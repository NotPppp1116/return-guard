#include "CFGValueFlow.hpp"

#include "Analyzer.hpp"
#include "AstUtils.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/Stmt.h>
#include <clang/Analysis/CFG.h>
#include <llvm/Support/Casting.h>

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace returnguard::internal {
namespace {

enum class Origin {
    No,
    Yes,
    Maybe,
};

Origin join_origin(Origin lhs, Origin rhs) { return lhs == rhs ? lhs : Origin::Maybe; }

struct StorageLocation {
    std::string key;
    bool pointer_like = false;

    bool operator==(const StorageLocation& other) const { return key == other.key; }
};

struct StorageLocationHash {
    std::size_t operator()(const StorageLocation& location) const {
        return std::hash<std::string>{}(location.key);
    }
};

using StorageSet = std::unordered_set<StorageLocation, StorageLocationHash>;

StorageLocation declaration_location(const clang::ValueDecl& declaration) {
    return {
        .key = "decl:" + std::to_string(reinterpret_cast<std::uintptr_t>(&declaration)),
        .pointer_like = declaration.getType()->isPointerType(),
    };
}

StorageLocation append_component(StorageLocation base, const clang::ValueDecl& component,
                                 bool pointer_like) {
    base.key += ".decl:";
    base.key += std::to_string(reinterpret_cast<std::uintptr_t>(&component));
    base.pointer_like = pointer_like;
    return base;
}

StorageLocation append_array_element(StorageLocation base, bool pointer_like) {
    base.key += "[]";
    base.pointer_like = pointer_like;
    return base;
}

struct PointsTo {
    StorageSet targets;
    bool unknown = false;

    bool operator==(const PointsTo&) const = default;

    bool join(const PointsTo& other) {
        const std::size_t old_size = targets.size();
        targets.insert(other.targets.begin(), other.targets.end());
        const bool old_unknown = unknown;
        unknown = unknown || other.unknown;
        return targets.size() != old_size || unknown != old_unknown;
    }
};

struct State {
    std::unordered_map<StorageLocation, Origin, StorageLocationHash> values;
    std::unordered_map<StorageLocation, PointsTo, StorageLocationHash> pointers;
    bool unchecked_use = false;

    bool operator==(const State&) const = default;

    [[nodiscard]] Origin value(const StorageLocation& location) const {
        const auto iterator = values.find(location);
        return iterator == values.end() ? Origin::No : iterator->second;
    }

    [[nodiscard]] PointsTo pointer(const StorageLocation& location) const {
        const auto iterator = pointers.find(location);
        if (iterator != pointers.end()) {
            return iterator->second;
        }
        return {
            .targets = {},
            .unknown = location.pointer_like,
        };
    }

    bool join(const State& other) {
        bool changed = false;

        StorageSet value_locations;
        for (const auto& [location, unused] : values) {
            (void)unused;
            value_locations.insert(location);
        }
        for (const auto& [location, unused] : other.values) {
            (void)unused;
            value_locations.insert(location);
        }

        for (const StorageLocation& location : value_locations) {
            const Origin old = value(location);
            const Origin merged = join_origin(old, other.value(location));
            changed = changed || old != merged;
            if (merged == Origin::No) {
                values.erase(location);
            } else {
                values[location] = merged;
            }
        }

        StorageSet pointer_locations;
        for (const auto& [location, unused] : pointers) {
            (void)unused;
            pointer_locations.insert(location);
        }
        for (const auto& [location, unused] : other.pointers) {
            (void)unused;
            pointer_locations.insert(location);
        }

        for (const StorageLocation& location : pointer_locations) {
            const PointsTo old = pointer(location);
            PointsTo merged = old;
            merged.join(other.pointer(location));
            changed = changed || merged != old;
            pointers[location] = std::move(merged);
        }
        const bool old_unchecked_use = unchecked_use;
        unchecked_use = unchecked_use || other.unchecked_use;
        changed = changed || unchecked_use != old_unchecked_use;
        return changed;
    }
};

struct Evaluation {
    Origin origin = Origin::No;
    PointsTo pointer;
    StorageSet locations;
    bool unknown_location = false;
};

Origin read_origin(const State& state, const StorageSet& locations) {
    std::optional<Origin> result;
    for (const StorageLocation& location : locations) {
        result = result.has_value() ? join_origin(*result, state.value(location))
                                    : state.value(location);
    }
    return result.value_or(Origin::No);
}

bool has_tracked_origin(const State& state) {
    for (const auto& [unused, origin] : state.values) {
        (void)unused;
        if (origin != Origin::No) {
            return true;
        }
    }
    return false;
}

void clear_tracked_origins(State& state) {
    for (auto iterator = state.values.begin(); iterator != state.values.end();) {
        if (iterator->second == Origin::No) {
            ++iterator;
            continue;
        }
        iterator = state.values.erase(iterator);
    }
}

PointsTo read_pointer(const State& state, const StorageSet& locations) {
    PointsTo result;
    for (const StorageLocation& location : locations) {
        result.join(state.pointer(location));
    }
    return result;
}

class Evaluator final {
  public:
    Evaluator(State& state, const clang::CallExpr* target, ExpressionSet* aliases,
              bool track_unchecked_uses = false)
        : state_(state), target_(target), aliases_(aliases),
          track_unchecked_uses_(track_unchecked_uses) {}

    Evaluation evaluate(const clang::Expr* expression) {
        if (expression == nullptr) {
            return {};
        }

        Evaluation result = evaluate_impl(expression);
        if (aliases_ != nullptr && result.origin == Origin::Yes) {
            if (const clang::Expr* canonical = strip_expr(expression)) {
                aliases_->insert(canonical);
            }
        }
        return result;
    }

    void process(const clang::Stmt* statement) {
        if (statement == nullptr) {
            return;
        }

        if (const auto* declaration_statement = llvm::dyn_cast<clang::DeclStmt>(statement)) {
            for (const clang::Decl* declaration : declaration_statement->decls()) {
                const auto* variable = llvm::dyn_cast<clang::VarDecl>(declaration);
                if (variable == nullptr || variable->getInit() == nullptr) {
                    continue;
                }
                const Evaluation initializer = evaluate(variable->getInit());
                write(*variable, initializer);
            }
            return;
        }

        if (const auto* return_statement = llvm::dyn_cast<clang::ReturnStmt>(statement)) {
            const Evaluation result = evaluate(return_statement->getRetValue());
            if (result.origin != Origin::No) {
                clear_tracked_origins(state_);
            }
            return;
        }

        if (const auto* expression = llvm::dyn_cast<clang::Expr>(statement)) {
            evaluate(expression);
        }
    }

  private:
    Evaluation evaluate_impl(const clang::Expr* expression) {
        if (target_ != nullptr && expression == target_) {
            for (const clang::Expr* argument : target_->arguments()) {
                evaluate(argument);
            }
            Evaluation result;
            result.origin = Origin::Yes;
            return result;
        }

        if (const auto* paren = llvm::dyn_cast<clang::ParenExpr>(expression)) {
            return evaluate(paren->getSubExpr());
        }

        if (const auto* cleanup = llvm::dyn_cast<clang::ExprWithCleanups>(expression)) {
            return evaluate(cleanup->getSubExpr());
        }

        if (const auto* cast = llvm::dyn_cast<clang::CastExpr>(expression)) {
            Evaluation result = evaluate(cast->getSubExpr());
            result.locations.clear();
            result.unknown_location = false;
            return result;
        }

        if (const auto* opaque = llvm::dyn_cast<clang::OpaqueValueExpr>(expression)) {
            return evaluate(opaque->getSourceExpr());
        }

        if (const auto* reference = llvm::dyn_cast<clang::DeclRefExpr>(expression)) {
            const auto* declaration = llvm::dyn_cast<clang::ValueDecl>(reference->getDecl());
            if (declaration == nullptr) {
                return {};
            }
            const StorageLocation location = declaration_location(*declaration);
            Evaluation result;
            result.origin = state_.value(location);
            result.pointer = state_.pointer(location);
            result.locations.insert(location);
            return result;
        }

        if (const auto* member = llvm::dyn_cast<clang::MemberExpr>(expression)) {
            const clang::ValueDecl* declaration = member->getMemberDecl();
            if (declaration == nullptr) {
                evaluate(member->getBase());
                return {};
            }

            Evaluation base = evaluate(member->getBase());
            Evaluation result;
            result.unknown_location = base.unknown_location;
            if (!base.unknown_location) {
                for (const StorageLocation& location : base.locations) {
                    result.locations.insert(append_component(location, *declaration,
                                                             member->getType()->isPointerType()));
                }
            }
            result.origin =
                result.unknown_location ? Origin::Maybe : read_origin(state_, result.locations);
            result.pointer = read_pointer(state_, result.locations);
            result.pointer.unknown = result.pointer.unknown || result.unknown_location;
            return result;
        }

        if (const auto* subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(expression)) {
            Evaluation base = evaluate(subscript->getBase());
            evaluate(subscript->getIdx());

            Evaluation result;
            result.unknown_location = base.unknown_location;
            if (!base.unknown_location) {
                for (const StorageLocation& location : base.locations) {
                    result.locations.insert(
                        append_array_element(location, subscript->getType()->isPointerType()));
                }
            }
            result.origin =
                result.unknown_location ? Origin::Maybe : read_origin(state_, result.locations);
            result.pointer = read_pointer(state_, result.locations);
            result.pointer.unknown = result.pointer.unknown || result.unknown_location;
            return result;
        }

        if (const auto* unary = llvm::dyn_cast<clang::UnaryOperator>(expression)) {
            Evaluation operand = evaluate(unary->getSubExpr());
            switch (unary->getOpcode()) {
            case clang::UO_AddrOf:
                return {
                    .origin = Origin::No,
                    .pointer =
                        {
                            .targets = std::move(operand.locations),
                            .unknown = operand.unknown_location,
                        },
                    .locations = {},
                    .unknown_location = false,
                };
            case clang::UO_Deref: {
                Evaluation result;
                result.locations = operand.pointer.targets;
                result.unknown_location = operand.pointer.unknown;
                result.origin =
                    result.unknown_location ? Origin::Maybe : read_origin(state_, result.locations);
                result.pointer = read_pointer(state_, result.locations);
                result.pointer.unknown = result.pointer.unknown || result.unknown_location;
                return result;
            }
            case clang::UO_PreInc:
            case clang::UO_PreDec:
            case clang::UO_PostInc:
            case clang::UO_PostDec:
                for (const StorageLocation& location : operand.locations) {
                    state_.values.erase(location);
                }
                return {};
            default:
                return {};
            }
        }

        if (const auto* binary = llvm::dyn_cast<clang::BinaryOperator>(expression)) {
            if (binary->getOpcode() == clang::BO_Comma) {
                evaluate(binary->getLHS());
                return evaluate(binary->getRHS());
            }

            if (binary->getOpcode() == clang::BO_Assign) {
                Evaluation lhs = evaluate(binary->getLHS());
                Evaluation rhs = evaluate(binary->getRHS());
                write(lhs.locations, rhs);
                rhs.locations.clear();
                rhs.unknown_location = false;
                return rhs;
            }

            const Evaluation lhs = evaluate(binary->getLHS());
            const Evaluation rhs = evaluate(binary->getRHS());
            if (track_unchecked_uses_ && !binary->isComparisonOp() &&
                !binary->isLogicalOp() &&
                (lhs.origin != Origin::No || rhs.origin != Origin::No)) {
                state_.unchecked_use = true;
            }
            return {};
        }

        if (const auto* conditional = llvm::dyn_cast<clang::ConditionalOperator>(expression)) {
            evaluate(conditional->getCond());

            State true_state = state_;
            Evaluator true_evaluator(true_state, target_, aliases_);
            Evaluation true_result = true_evaluator.evaluate(conditional->getTrueExpr());

            State false_state = state_;
            Evaluator false_evaluator(false_state, target_, aliases_);
            Evaluation false_result = false_evaluator.evaluate(conditional->getFalseExpr());

            state_ = std::move(true_state);
            state_.join(false_state);

            Evaluation result;
            result.origin = join_origin(true_result.origin, false_result.origin);
            result.pointer = std::move(true_result.pointer);
            result.pointer.join(false_result.pointer);
            result.locations.insert(true_result.locations.begin(), true_result.locations.end());
            result.locations.insert(false_result.locations.begin(), false_result.locations.end());
            result.unknown_location = true_result.unknown_location || false_result.unknown_location;
            return result;
        }

        if (const auto* call = llvm::dyn_cast<clang::CallExpr>(expression)) {
            for (const clang::Expr* argument : call->arguments()) {
                evaluate(argument);
            }
            return {};
        }

        for (const clang::Stmt* child : expression->children()) {
            if (const auto* child_expression = llvm::dyn_cast_or_null<clang::Expr>(child)) {
                evaluate(child_expression);
            }
        }
        return {};
    }

    void write(const clang::ValueDecl& declaration, const Evaluation& value) {
        const StorageSet locations = {declaration_location(declaration)};
        write(locations, value);
    }

    void write(const StorageSet& locations, const Evaluation& value) {
        for (const StorageLocation& location : locations) {
            if (value.origin == Origin::No) {
                state_.values.erase(location);
            } else {
                state_.values[location] = value.origin;
            }

            if (location.pointer_like) {
                state_.pointers[location] = value.pointer;
            } else {
                state_.pointers.erase(location);
            }
        }
    }

    State& state_;
    const clang::CallExpr* target_;
    ExpressionSet* aliases_;
    bool track_unchecked_uses_;
};

void transfer_block(const clang::CFGBlock& block, State& state, const clang::CallExpr* target,
                    ExpressionSet* aliases) {
    Evaluator evaluator(state, target, aliases);
    for (const clang::CFGElement& element : block) {
        if (const auto statement = element.getAs<clang::CFGStmt>()) {
            evaluator.process(statement->getStmt());
        }
    }

    if (aliases != nullptr) {
        if (const auto* condition =
                llvm::dyn_cast_or_null<clang::Expr>(block.getTerminatorCondition())) {
            evaluator.evaluate(condition);
        }
    }
}

void transfer_block_for_live_paths(const clang::CFGBlock& block, State& state,
                                   const clang::CallExpr& target, Analyzer& analyzer,
                                   const Domain& domain) {
    Evaluator evaluator(state, &target, nullptr, true);
    for (const clang::CFGElement& element : block) {
        if (const auto statement = element.getAs<clang::CFGStmt>()) {
            evaluator.process(statement->getStmt());
        }
    }

    const auto* terminator = block.getTerminatorStmt();
    const auto* condition = llvm::dyn_cast_or_null<clang::Expr>(block.getTerminatorCondition());
    if (condition == nullptr) {
        if (const auto* if_statement = llvm::dyn_cast_or_null<clang::IfStmt>(terminator)) {
            condition = if_statement->getCond();
        } else if (const auto* switch_statement =
                       llvm::dyn_cast_or_null<clang::SwitchStmt>(terminator)) {
            condition = switch_statement->getCond();
        }
    }
    if (condition == nullptr || !has_tracked_origin(state)) {
        return;
    }

    ExpressionSet aliases;
    Evaluator condition_evaluator(state, &target, &aliases);
    condition_evaluator.evaluate(condition);
    if (aliases.empty()) {
        return;
    }

    CheckResult result;
    result.kind = HandlingKind::PartiallyChecked;
    if (terminator != nullptr) {
        if (const auto* if_statement = llvm::dyn_cast<clang::IfStmt>(terminator)) {
            result = analyzer.analyze_if_chain(if_statement, aliases, domain);
        } else if (const auto* switch_statement = llvm::dyn_cast<clang::SwitchStmt>(terminator)) {
            result = analyzer.analyze_switch(switch_statement, domain);
        } else {
            result = analyzer.analyze_condition(condition, aliases, domain);
        }
    } else {
        result = analyzer.analyze_condition(condition, aliases, domain);
    }

    if (result.kind == HandlingKind::ExhaustivelyChecked) {
        clear_tracked_origins(state);
    }
}

} // namespace

CFGValueFlow::CFGValueFlow(std::unique_ptr<clang::CFG> cfg, clang::ASTContext& context)
    : cfg_(std::move(cfg)), context_(context) {}

std::unique_ptr<CFGValueFlow> CFGValueFlow::build(const clang::FunctionDecl& function,
                                                  clang::ASTContext& context) {
    if (!function.doesThisDeclarationHaveABody()) {
        return nullptr;
    }

    clang::CFG::BuildOptions options;
    options.PruneTriviallyFalseEdges = true;
    options.AddInitializers = true;

    std::unique_ptr<clang::CFG> cfg =
        clang::CFG::buildCFG(&function, function.getBody(), &context, options);
    if (!cfg) {
        return nullptr;
    }
    return std::unique_ptr<CFGValueFlow>(new CFGValueFlow(std::move(cfg), context));
}

ExpressionSet CFGValueFlow::aliases_for(const clang::CallExpr& call) const {
    (void)context_;
    const unsigned block_count = cfg_->getNumBlockIDs();
    std::vector<std::optional<State>> inputs(block_count);
    std::deque<const clang::CFGBlock*> worklist;

    const clang::CFGBlock& entry = cfg_->getEntry();
    inputs[entry.getBlockID()] = State{};
    worklist.push_back(&entry);

    while (!worklist.empty()) {
        const clang::CFGBlock* block = worklist.front();
        worklist.pop_front();

        State output = *inputs[block->getBlockID()];
        transfer_block(*block, output, &call, nullptr);

        for (const clang::CFGBlock* successor : block->succs()) {
            if (successor == nullptr) {
                continue;
            }

            std::optional<State>& input = inputs[successor->getBlockID()];
            if (!input.has_value()) {
                input = output;
                worklist.push_back(successor);
                continue;
            }

            State merged = *input;
            if (merged.join(output)) {
                input = std::move(merged);
                worklist.push_back(successor);
            }
        }
    }

    ExpressionSet aliases;
    for (const clang::CFGBlock* block : *cfg_) {
        if (block == nullptr || !inputs[block->getBlockID()].has_value()) {
            continue;
        }
        State state = *inputs[block->getBlockID()];
        transfer_block(*block, state, &call, &aliases);
    }
    aliases.erase(strip_expr(&call));
    return aliases;
}

ExpressionSet CFGValueFlow::aliases_for(const clang::ValueDecl& declaration) const {
    (void)context_;
    const unsigned block_count = cfg_->getNumBlockIDs();
    std::vector<std::optional<State>> inputs(block_count);
    std::deque<const clang::CFGBlock*> worklist;

    State entry_state;
    entry_state.values[declaration_location(declaration)] = Origin::Yes;

    const clang::CFGBlock& entry = cfg_->getEntry();
    inputs[entry.getBlockID()] = std::move(entry_state);
    worklist.push_back(&entry);

    while (!worklist.empty()) {
        const clang::CFGBlock* block = worklist.front();
        worklist.pop_front();

        State output = *inputs[block->getBlockID()];
        transfer_block(*block, output, nullptr, nullptr);

        for (const clang::CFGBlock* successor : block->succs()) {
            if (successor == nullptr) {
                continue;
            }

            std::optional<State>& input = inputs[successor->getBlockID()];
            if (!input.has_value()) {
                input = output;
                worklist.push_back(successor);
                continue;
            }

            State merged = *input;
            if (merged.join(output)) {
                input = std::move(merged);
                worklist.push_back(successor);
            }
        }
    }

    ExpressionSet aliases;
    for (const clang::CFGBlock* block : *cfg_) {
        if (block == nullptr || !inputs[block->getBlockID()].has_value()) {
            continue;
        }
        State state = *inputs[block->getBlockID()];
        transfer_block(*block, state, nullptr, &aliases);
    }
    return aliases;
}

bool CFGValueFlow::has_unhandled_live_path(const clang::CallExpr& call, Analyzer& analyzer,
                                           const Domain& domain) const {
    (void)context_;
    const unsigned block_count = cfg_->getNumBlockIDs();
    std::vector<std::optional<State>> inputs(block_count);
    std::deque<const clang::CFGBlock*> worklist;

    const clang::CFGBlock& entry = cfg_->getEntry();
    inputs[entry.getBlockID()] = State{};
    worklist.push_back(&entry);

    while (!worklist.empty()) {
        const clang::CFGBlock* block = worklist.front();
        worklist.pop_front();

        State output = *inputs[block->getBlockID()];
        transfer_block_for_live_paths(*block, output, call, analyzer, domain);

        for (const clang::CFGBlock* successor : block->succs()) {
            if (successor == nullptr) {
                continue;
            }

            std::optional<State>& input = inputs[successor->getBlockID()];
            if (!input.has_value()) {
                input = output;
                worklist.push_back(successor);
                continue;
            }

            State merged = *input;
            if (merged.join(output)) {
                input = std::move(merged);
                worklist.push_back(successor);
            }
        }
    }

    const clang::CFGBlock& exit = cfg_->getExit();
    const std::optional<State>& exit_input = inputs[exit.getBlockID()];
    return exit_input.has_value() && exit_input->unchecked_use && has_tracked_origin(*exit_input);
}

} // namespace returnguard::internal
