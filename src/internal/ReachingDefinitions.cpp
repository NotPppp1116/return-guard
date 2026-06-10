#include "ReachingDefinitions.hpp"

#include "AstUtils.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/Stmt.h>
#include <clang/Analysis/CFG.h>
#include <llvm/Support/Casting.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace returnguard::internal {
namespace {

struct State {
    bool unknown = true;
    std::unordered_set<const clang::Expr*> definitions;

    bool operator==(const State&) const = default;

    void assign(const clang::Expr* expression) {
        unknown = expression == nullptr;
        definitions.clear();
        if (expression != nullptr) {
            definitions.insert(expression);
        }
    }

    void invalidate() {
        unknown = true;
        definitions.clear();
    }

    bool join(const State& other) {
        const State old = *this;
        if (unknown || other.unknown) {
            invalidate();
        } else {
            definitions.insert(other.definitions.begin(), other.definitions.end());
        }
        return *this != old;
    }
};

bool is_nonconst_reference(clang::QualType type) {
    const auto* reference = type->getAs<clang::ReferenceType>();
    return reference != nullptr && !reference->getPointeeType().isConstQualified();
}

class Transfer final {
  public:
    Transfer(State& state,
             const clang::VarDecl& variable,
             const clang::Expr& reference,
             std::optional<State>* captured)
        : state_(state), variable_(variable), reference_(reference), captured_(captured) {}

    void process(const clang::Stmt* statement) {
        if (statement == nullptr) {
            return;
        }

        if (const auto* declaration_statement = llvm::dyn_cast<clang::DeclStmt>(statement)) {
            for (const clang::Decl* declaration : declaration_statement->decls()) {
                const auto* variable = llvm::dyn_cast<clang::VarDecl>(declaration);
                if (variable == nullptr) {
                    continue;
                }
                process_expression(variable->getInit());
                if (variable == &variable_) {
                    state_.assign(variable->getInit());
                }
            }
            return;
        }

        if (const auto* return_statement = llvm::dyn_cast<clang::ReturnStmt>(statement)) {
            process_expression(return_statement->getRetValue());
            return;
        }

        if (const auto* expression = llvm::dyn_cast<clang::Expr>(statement)) {
            process_expression(expression);
        }
    }

    void process_expression(const clang::Expr* expression) {
        if (expression == nullptr) {
            return;
        }

        if (expression == &reference_) {
            capture();
        }

        if (const auto* parentheses = llvm::dyn_cast<clang::ParenExpr>(expression)) {
            process_expression(parentheses->getSubExpr());
            return;
        }
        if (const auto* cleanup = llvm::dyn_cast<clang::ExprWithCleanups>(expression)) {
            process_expression(cleanup->getSubExpr());
            return;
        }
        if (const auto* cast = llvm::dyn_cast<clang::CastExpr>(expression)) {
            process_expression(cast->getSubExpr());
            return;
        }
        if (const auto* opaque = llvm::dyn_cast<clang::OpaqueValueExpr>(expression)) {
            process_expression(opaque->getSourceExpr());
            return;
        }
        if (const auto* temporary = llvm::dyn_cast<clang::MaterializeTemporaryExpr>(expression)) {
            process_expression(temporary->getSubExpr());
            return;
        }
        if (const auto* temporary = llvm::dyn_cast<clang::CXXBindTemporaryExpr>(expression)) {
            process_expression(temporary->getSubExpr());
            return;
        }
        if (llvm::isa<clang::UnaryExprOrTypeTraitExpr>(expression)) {
            return;
        }

        if (const auto* binary = llvm::dyn_cast<clang::BinaryOperator>(expression)) {
            process_binary(binary);
            return;
        }

        if (const auto* unary = llvm::dyn_cast<clang::UnaryOperator>(expression)) {
            process_unary(unary);
            return;
        }

        if (const auto* conditional = llvm::dyn_cast<clang::ConditionalOperator>(expression)) {
            process_expression(conditional->getCond());
            State true_state = state_;
            Transfer true_transfer(true_state, variable_, reference_, captured_);
            true_transfer.process_expression(conditional->getTrueExpr());
            State false_state = state_;
            Transfer false_transfer(false_state, variable_, reference_, captured_);
            false_transfer.process_expression(conditional->getFalseExpr());
            state_ = std::move(true_state);
            state_.join(false_state);
            return;
        }

        if (const auto* conditional = llvm::dyn_cast<clang::BinaryConditionalOperator>(expression)) {
            process_expression(conditional->getCond());
            State true_state = state_;
            Transfer true_transfer(true_state, variable_, reference_, captured_);
            true_transfer.process_expression(conditional->getCommon());
            State false_state = state_;
            Transfer false_transfer(false_state, variable_, reference_, captured_);
            false_transfer.process_expression(conditional->getFalseExpr());
            state_ = std::move(true_state);
            state_.join(false_state);
            return;
        }

        if (const auto* call = llvm::dyn_cast<clang::CallExpr>(expression)) {
            process_call(call);
            return;
        }

        if (const auto* construction = llvm::dyn_cast<clang::CXXConstructExpr>(expression)) {
            for (const clang::Expr* argument : construction->arguments()) {
                process_expression(argument);
            }
            const clang::CXXConstructorDecl* constructor = construction->getConstructor();
            if (constructor != nullptr) {
                const unsigned count = std::min(construction->getNumArgs(), constructor->getNumParams());
                for (unsigned index = 0; index < count; ++index) {
                    if (is_nonconst_reference(constructor->getParamDecl(index)->getType()) &&
                        expression_references_variable(construction->getArg(index), &variable_)) {
                        state_.invalidate();
                    }
                }
            }
            return;
        }

        if (const auto* lambda = llvm::dyn_cast<clang::LambdaExpr>(expression)) {
            for (const clang::LambdaCapture& capture : lambda->captures()) {
                if (capture.capturesVariable() && capture.getCapturedVar() == &variable_ &&
                    capture.getCaptureKind() == clang::LCK_ByRef) {
                    state_.invalidate();
                    break;
                }
            }
        }

        for (const clang::Stmt* child : expression->children()) {
            if (const auto* child_expression = llvm::dyn_cast_or_null<clang::Expr>(child)) {
                process_expression(child_expression);
            }
        }
    }

  private:
    void capture() {
        if (captured_ == nullptr) {
            return;
        }
        if (!captured_->has_value()) {
            *captured_ = state_;
            return;
        }
        captured_->value().join(state_);
    }

    void process_binary(const clang::BinaryOperator* binary) {
        if (binary->getOpcode() == clang::BO_LAnd || binary->getOpcode() == clang::BO_LOr) {
            process_expression(binary->getLHS());
            State short_circuit_state = state_;
            State rhs_state = state_;
            Transfer rhs_transfer(rhs_state, variable_, reference_, captured_);
            rhs_transfer.process_expression(binary->getRHS());
            state_ = std::move(short_circuit_state);
            state_.join(rhs_state);
            return;
        }

        if (!binary->isAssignmentOp()) {
            process_expression(binary->getLHS());
            process_expression(binary->getRHS());
            return;
        }

        const clang::VarDecl* assigned = referenced_variable(binary->getLHS());
        if (assigned != &variable_) {
            process_expression(binary->getLHS());
            process_expression(binary->getRHS());
            return;
        }

        if (binary->getOpcode() == clang::BO_Assign) {
            process_expression(binary->getRHS());
            state_.assign(binary->getRHS());
            return;
        }

        process_expression(binary->getLHS());
        process_expression(binary->getRHS());
        state_.invalidate();
    }

    void process_unary(const clang::UnaryOperator* unary) {
        const clang::VarDecl* operand = referenced_variable(unary->getSubExpr());
        if (unary->getOpcode() == clang::UO_AddrOf && operand == &variable_) {
            process_expression(unary->getSubExpr());
            state_.invalidate();
            return;
        }

        process_expression(unary->getSubExpr());
        if (operand == &variable_ && unary->isIncrementDecrementOp()) {
            state_.invalidate();
        }
    }

    void process_call(const clang::CallExpr* call) {
        process_expression(call->getCallee());
        for (const clang::Expr* argument : call->arguments()) {
            process_expression(argument);
        }

        const clang::FunctionDecl* callee = call->getDirectCallee();
        if (callee == nullptr) {
            return;
        }

        const unsigned count = std::min(call->getNumArgs(), callee->getNumParams());
        for (unsigned index = 0; index < count; ++index) {
            if (is_nonconst_reference(callee->getParamDecl(index)->getType()) &&
                expression_references_variable(call->getArg(index), &variable_)) {
                state_.invalidate();
                return;
            }
        }
    }

    State& state_;
    const clang::VarDecl& variable_;
    const clang::Expr& reference_;
    std::optional<State>* captured_;
};

void transfer_block(const clang::CFGBlock& block,
                    State& state,
                    const clang::VarDecl& variable,
                    const clang::Expr& reference,
                    std::optional<State>* captured) {
    Transfer transfer(state, variable, reference, captured);
    for (const clang::CFGElement& element : block) {
        if (const auto statement = element.getAs<clang::CFGStmt>()) {
            transfer.process(statement->getStmt());
        }
    }
    transfer.process_expression(block.getLastCondition());
}

} // namespace

ReachingDefinitions reaching_definitions_at(const clang::FunctionDecl& function,
                                             const clang::VarDecl& variable,
                                             const clang::Expr& reference,
                                             clang::ASTContext& context) {
    clang::CFG::BuildOptions options;
    options.PruneTriviallyFalseEdges = true;
    options.AddInitializers = true;

    std::unique_ptr<clang::CFG> cfg =
        clang::CFG::buildCFG(&function, function.getBody(), &context, options);
    if (!cfg) {
        return {.unknown = true, .expressions = {}};
    }

    const unsigned block_count = cfg->getNumBlockIDs();
    std::vector<std::optional<State>> inputs(block_count);
    std::deque<const clang::CFGBlock*> worklist;

    const clang::CFGBlock& entry = cfg->getEntry();
    inputs[entry.getBlockID()] = State{};
    worklist.push_back(&entry);

    while (!worklist.empty()) {
        const clang::CFGBlock* block = worklist.front();
        worklist.pop_front();

        State output = *inputs[block->getBlockID()];
        transfer_block(*block, output, variable, reference, nullptr);

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

    std::optional<State> captured;
    for (const clang::CFGBlock* block : *cfg) {
        if (block == nullptr || !inputs[block->getBlockID()].has_value()) {
            continue;
        }
        State state = *inputs[block->getBlockID()];
        transfer_block(*block, state, variable, reference, &captured);
    }

    if (!captured.has_value() || captured->unknown || captured->definitions.empty()) {
        return {.unknown = true, .expressions = {}};
    }

    std::vector<const clang::Expr*> expressions(captured->definitions.begin(),
                                                captured->definitions.end());
    return {.unknown = false, .expressions = std::move(expressions)};
}

} // namespace returnguard::internal
