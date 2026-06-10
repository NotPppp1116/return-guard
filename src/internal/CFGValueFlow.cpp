#include "CFGValueFlow.hpp"

#include "AstUtils.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/Stmt.h>
#include <clang/Analysis/CFG.h>
#include <llvm/Support/Casting.h>

#include <deque>
#include <optional>
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

Origin join_origin(Origin lhs, Origin rhs) {
    return lhs == rhs ? lhs : Origin::Maybe;
}

struct PointsTo {
    std::unordered_set<const clang::ValueDecl*> targets;
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
    std::unordered_map<const clang::ValueDecl*, Origin> values;
    std::unordered_map<const clang::ValueDecl*, PointsTo> pointers;

    bool operator==(const State&) const = default;

    [[nodiscard]] Origin value(const clang::ValueDecl* declaration) const {
        const auto iterator = values.find(declaration);
        return iterator == values.end() ? Origin::No : iterator->second;
    }

    [[nodiscard]] PointsTo pointer(const clang::ValueDecl* declaration) const {
        const auto iterator = pointers.find(declaration);
        if (iterator != pointers.end()) {
            return iterator->second;
        }
        return {
            .targets = {},
            .unknown = declaration != nullptr && declaration->getType()->isPointerType(),
        };
    }

    bool join(const State& other) {
        bool changed = false;

        std::unordered_set<const clang::ValueDecl*> value_declarations;
        for (const auto& [declaration, unused] : values) {
            (void)unused;
            value_declarations.insert(declaration);
        }
        for (const auto& [declaration, unused] : other.values) {
            (void)unused;
            value_declarations.insert(declaration);
        }

        for (const clang::ValueDecl* declaration : value_declarations) {
            const Origin old = value(declaration);
            const Origin merged = join_origin(old, other.value(declaration));
            changed = changed || old != merged;
            if (merged == Origin::No) {
                values.erase(declaration);
            } else {
                values[declaration] = merged;
            }
        }

        std::unordered_set<const clang::ValueDecl*> pointer_declarations;
        for (const auto& [declaration, unused] : pointers) {
            (void)unused;
            pointer_declarations.insert(declaration);
        }
        for (const auto& [declaration, unused] : other.pointers) {
            (void)unused;
            pointer_declarations.insert(declaration);
        }

        for (const clang::ValueDecl* declaration : pointer_declarations) {
            const PointsTo old = pointer(declaration);
            PointsTo merged = old;
            merged.join(other.pointer(declaration));
            changed = changed || merged != old;
            pointers[declaration] = std::move(merged);
        }
        return changed;
    }
};

struct Evaluation {
    Origin origin = Origin::No;
    PointsTo pointer;
    std::unordered_set<const clang::ValueDecl*> locations;
    bool unknown_location = false;
};

Origin read_origin(
    const State& state,
    const std::unordered_set<const clang::ValueDecl*>& locations) {
    std::optional<Origin> result;
    for (const clang::ValueDecl* location : locations) {
        result = result.has_value()
                     ? join_origin(*result, state.value(location))
                     : state.value(location);
    }
    return result.value_or(Origin::No);
}

PointsTo read_pointer(
    const State& state,
    const std::unordered_set<const clang::ValueDecl*>& locations) {
    PointsTo result;
    for (const clang::ValueDecl* location : locations) {
        result.join(state.pointer(location));
    }
    return result;
}

class Evaluator final {
  public:
    Evaluator(
        State& state,
        const clang::CallExpr& target,
        ExpressionSet* aliases)
        : state_(state), target_(target), aliases_(aliases) {}

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

        if (const auto* declaration_statement =
                llvm::dyn_cast<clang::DeclStmt>(statement)) {
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

        if (const auto* return_statement =
                llvm::dyn_cast<clang::ReturnStmt>(statement)) {
            evaluate(return_statement->getRetValue());
            return;
        }

        if (const auto* expression = llvm::dyn_cast<clang::Expr>(statement)) {
            evaluate(expression);
        }
    }

  private:
    Evaluation evaluate_impl(const clang::Expr* expression) {
        if (expression == &target_) {
            for (const clang::Expr* argument : target_.arguments()) {
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
            Evaluation result;
            result.origin = state_.value(declaration);
            result.pointer = state_.pointer(declaration);
            result.locations.insert(declaration);
            return result;
        }

        if (const auto* unary = llvm::dyn_cast<clang::UnaryOperator>(expression)) {
            Evaluation operand = evaluate(unary->getSubExpr());
            switch (unary->getOpcode()) {
            case clang::UO_AddrOf:
                return {
                    .origin = Origin::No,
                    .pointer = {
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
                result.origin = result.unknown_location
                                    ? Origin::Maybe
                                    : read_origin(state_, result.locations);
                result.pointer = read_pointer(state_, result.locations);
                result.pointer.unknown = result.pointer.unknown || result.unknown_location;
                return result;
            }
            case clang::UO_PreInc:
            case clang::UO_PreDec:
            case clang::UO_PostInc:
            case clang::UO_PostDec:
                for (const clang::ValueDecl* location : operand.locations) {
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

            evaluate(binary->getLHS());
            evaluate(binary->getRHS());
            return {};
        }

        if (const auto* conditional =
                llvm::dyn_cast<clang::ConditionalOperator>(expression)) {
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
            result.locations.insert(
                true_result.locations.begin(),
                true_result.locations.end());
            result.locations.insert(
                false_result.locations.begin(),
                false_result.locations.end());
            result.unknown_location =
                true_result.unknown_location || false_result.unknown_location;
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
        const std::unordered_set<const clang::ValueDecl*> locations = {&declaration};
        write(locations, value);
    }

    void write(
        const std::unordered_set<const clang::ValueDecl*>& locations,
        const Evaluation& value) {
        for (const clang::ValueDecl* location : locations) {
            if (value.origin == Origin::No) {
                state_.values.erase(location);
            } else {
                state_.values[location] = value.origin;
            }

            if (location->getType()->isPointerType()) {
                state_.pointers[location] = value.pointer;
            } else {
                state_.pointers.erase(location);
            }
        }
    }

    State& state_;
    const clang::CallExpr& target_;
    ExpressionSet* aliases_;
};

void transfer_block(
    const clang::CFGBlock& block,
    State& state,
    const clang::CallExpr& target,
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

} // namespace

CFGValueFlow::CFGValueFlow(
    std::unique_ptr<clang::CFG> cfg,
    clang::ASTContext& context)
    : cfg_(std::move(cfg)), context_(context) {}

std::unique_ptr<CFGValueFlow> CFGValueFlow::build(
    const clang::FunctionDecl& function,
    clang::ASTContext& context) {
    if (!function.doesThisDeclarationHaveABody()) {
        return nullptr;
    }

    clang::CFG::BuildOptions options;
    options.PruneTriviallyFalseEdges = true;
    options.AddInitializers = true;

    std::unique_ptr<clang::CFG> cfg = clang::CFG::buildCFG(
        &function,
        function.getBody(),
        &context,
        options);
    if (!cfg) {
        return nullptr;
    }
    return std::unique_ptr<CFGValueFlow>(
        new CFGValueFlow(std::move(cfg), context));
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
        transfer_block(*block, output, call, nullptr);

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
        transfer_block(*block, state, call, &aliases);
    }
    aliases.erase(strip_expr(&call));
    return aliases;
}

} // namespace returnguard::internal
