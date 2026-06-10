#include "Analyzer.hpp"
#include "AstUtils.hpp"
#include "NullStateAnalysis.hpp"

#include <returnguard/Options.hpp>

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/Analysis/CFG.h>
#include <clang/Basic/Diagnostic.h>
#include <llvm/Support/Casting.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace returnguard::internal {
namespace {

enum class TokenState {
    Live,
    Freed,
    MaybeFreed,
};

TokenState join_token_state(TokenState lhs, TokenState rhs) {
    return lhs == rhs ? lhs : TokenState::MaybeFreed;
}

struct PointerValue {
    std::unordered_set<const clang::ValueDecl*> tokens;

    bool operator==(const PointerValue&) const = default;
};

struct LifetimeState {
    std::unordered_map<const clang::ValueDecl*, PointerValue> values;
    std::unordered_map<const clang::ValueDecl*, TokenState> token_states;
    std::unordered_map<const clang::ValueDecl*, const clang::Stmt*> freed_at;

    bool operator==(const LifetimeState&) const = default;

    [[nodiscard]] PointerValue value(const clang::ValueDecl* declaration) const {
        const auto iterator = values.find(declaration);
        return iterator == values.end() ? PointerValue{} : iterator->second;
    }

    [[nodiscard]] TokenState token_state(const clang::ValueDecl* token) const {
        const auto iterator = token_states.find(token);
        return iterator == token_states.end() ? TokenState::Live : iterator->second;
    }

    void set(const clang::ValueDecl* declaration, PointerValue value) {
        if (declaration == nullptr || !declaration->getType()->isPointerType() ||
            value.tokens.empty()) {
            values.erase(declaration);
            return;
        }
        values[declaration] = std::move(value);
    }

    void set_fresh(const clang::ValueDecl* declaration) {
        if (declaration == nullptr || !declaration->getType()->isPointerType()) {
            return;
        }
        values[declaration] = PointerValue{.tokens = {declaration}};
        token_states[declaration] = TokenState::Live;
        freed_at.erase(declaration);
    }

    void kill(const clang::ValueDecl* declaration) { values.erase(declaration); }

    void mark_freed(const PointerValue& value, const clang::Stmt* statement) {
        for (const clang::ValueDecl* token : value.tokens) {
            token_states[token] = TokenState::Freed;
            freed_at[token] = statement;
        }
    }

    [[nodiscard]] std::optional<const clang::Stmt*> freed_note(const PointerValue& value) const {
        for (const clang::ValueDecl* token : value.tokens) {
            const auto iterator = freed_at.find(token);
            if (iterator != freed_at.end()) {
                return iterator->second;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] bool maybe_freed(const PointerValue& value) const {
        return std::any_of(value.tokens.begin(), value.tokens.end(),
                           [&](const clang::ValueDecl* token) {
                               const TokenState state = token_state(token);
                               return state == TokenState::Freed || state == TokenState::MaybeFreed;
                           });
    }

    bool join(const LifetimeState& other) {
        bool changed = false;

        std::unordered_set<const clang::ValueDecl*> declarations;
        for (const auto& [declaration, unused] : values) {
            (void)unused;
            declarations.insert(declaration);
        }
        for (const auto& [declaration, unused] : other.values) {
            (void)unused;
            declarations.insert(declaration);
        }

        for (const clang::ValueDecl* declaration : declarations) {
            PointerValue merged = value(declaration);
            const std::size_t old_size = merged.tokens.size();
            const PointerValue rhs = other.value(declaration);
            merged.tokens.insert(rhs.tokens.begin(), rhs.tokens.end());
            changed = changed || old_size != merged.tokens.size();
            set(declaration, std::move(merged));
        }

        std::unordered_set<const clang::ValueDecl*> tokens;
        for (const auto& [token, unused] : token_states) {
            (void)unused;
            tokens.insert(token);
        }
        for (const auto& [token, unused] : other.token_states) {
            (void)unused;
            tokens.insert(token);
        }

        for (const clang::ValueDecl* token : tokens) {
            const TokenState old = token_state(token);
            const TokenState merged = join_token_state(old, other.token_state(token));
            changed = changed || old != merged;
            token_states[token] = merged;
            if (!freed_at.contains(token)) {
                if (const auto found = other.freed_at.find(token); found != other.freed_at.end()) {
                    freed_at[token] = found->second;
                }
            }
        }

        return changed;
    }
};

const clang::ValueDecl* lvalue_declaration(const clang::Expr* expression) {
    expression = strip_expr(expression);
    if (const auto* reference = llvm::dyn_cast_or_null<clang::DeclRefExpr>(expression)) {
        return llvm::dyn_cast<clang::ValueDecl>(reference->getDecl());
    }
    return nullptr;
}

bool is_allocation_call(const clang::CallExpr* call) {
    const clang::FunctionDecl* callee = call == nullptr ? nullptr : call->getDirectCallee();
    if (callee == nullptr) {
        return false;
    }
    const llvm::StringRef name = callee->getName();
    return name == "malloc" || name == "calloc" || name == "aligned_alloc";
}

bool returns_pointer(const clang::Expr* expression) {
    return expression != nullptr && !expression->getType().isNull() &&
           expression->getType()->isPointerType();
}

const clang::Expr* ignore_pointer_casts(const clang::Expr* expression) {
    return expression == nullptr ? nullptr : expression->IgnoreParenCasts();
}

bool is_deallocation_call(const clang::CallExpr* call) {
    const clang::FunctionDecl* callee = call == nullptr ? nullptr : call->getDirectCallee();
    if (callee == nullptr) {
        return false;
    }
    const llvm::StringRef name = callee->getName();
    return name == "free" || name == "realloc";
}

bool is_realloc_call(const clang::CallExpr* call) {
    const clang::FunctionDecl* callee = call == nullptr ? nullptr : call->getDirectCallee();
    return callee != nullptr && callee->getName() == "realloc";
}

class LifetimeTransfer final {
  public:
    LifetimeTransfer(LifetimeState& state, clang::ASTContext& context,
                     std::unordered_set<const clang::Stmt*>* emitted_uaf,
                     std::unordered_set<const clang::Stmt*>* emitted_double_free)
        : state_(state), context_(context), emitted_uaf_(emitted_uaf),
          emitted_double_free_(emitted_double_free) {}

    void process(const clang::Stmt* statement) {
        if (statement == nullptr) {
            return;
        }
        if (const auto* declarations = llvm::dyn_cast<clang::DeclStmt>(statement)) {
            for (const clang::Decl* declaration : declarations->decls()) {
                const auto* variable = llvm::dyn_cast<clang::VarDecl>(declaration);
                if (variable == nullptr) {
                    continue;
                }
                if (!variable->getType()->isPointerType()) {
                    evaluate(variable->getInit());
                    continue;
                }
                if (variable->getInit() == nullptr) {
                    state_.kill(variable);
                    continue;
                }
                assign(variable, variable->getInit());
            }
            return;
        }
        if (const auto* return_statement = llvm::dyn_cast<clang::ReturnStmt>(statement)) {
            evaluate(return_statement->getRetValue());
            return;
        }
        if (const auto* expression = llvm::dyn_cast<clang::Expr>(statement)) {
            evaluate(expression);
        }
    }

  private:
    PointerValue evaluate(const clang::Expr* expression) {
        expression = strip_expr(expression);
        if (expression == nullptr) {
            return {};
        }

        if (const auto* reference = llvm::dyn_cast<clang::DeclRefExpr>(expression)) {
            const auto* declaration = llvm::dyn_cast<clang::ValueDecl>(reference->getDecl());
            PointerValue value = state_.value(declaration);
            if (value.tokens.empty() && declaration != nullptr &&
                declaration->getType()->isPointerType() &&
                !llvm::isa<clang::FunctionDecl>(declaration)) {
                state_.set_fresh(declaration);
                value = state_.value(declaration);
            }
            return value;
        }

        if (const auto* cast = llvm::dyn_cast<clang::CastExpr>(expression)) {
            return evaluate(cast->getSubExpr());
        }
        if (const auto* cleanup = llvm::dyn_cast<clang::ExprWithCleanups>(expression)) {
            return evaluate(cleanup->getSubExpr());
        }
        if (const auto* opaque = llvm::dyn_cast<clang::OpaqueValueExpr>(expression)) {
            return evaluate(opaque->getSourceExpr());
        }
        if (const auto* temporary = llvm::dyn_cast<clang::MaterializeTemporaryExpr>(expression)) {
            return evaluate(temporary->getSubExpr());
        }
        if (const auto* temporary = llvm::dyn_cast<clang::CXXBindTemporaryExpr>(expression)) {
            return evaluate(temporary->getSubExpr());
        }

        if (const auto* unary = llvm::dyn_cast<clang::UnaryOperator>(expression)) {
            if (unary->getOpcode() == clang::UO_Deref) {
                PointerValue pointer = evaluate(unary->getSubExpr());
                diagnose_uaf(expression, pointer);
                return pointer;
            }
            if (unary->getOpcode() == clang::UO_AddrOf) {
                return {};
            }
            return evaluate(unary->getSubExpr());
        }

        if (const auto* subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(expression)) {
            PointerValue base = evaluate(subscript->getBase());
            diagnose_uaf(expression, base);
            evaluate(subscript->getIdx());
            return base;
        }

        if (const auto* member = llvm::dyn_cast<clang::MemberExpr>(expression)) {
            PointerValue base = evaluate(member->getBase());
            if (member->isArrow()) {
                diagnose_uaf(expression, base);
            }
            return {};
        }

        if (const auto* binary = llvm::dyn_cast<clang::BinaryOperator>(expression)) {
            if (binary->getOpcode() == clang::BO_Assign) {
                if (const clang::ValueDecl* target = lvalue_declaration(binary->getLHS())) {
                    assign(target, binary->getRHS());
                    return state_.value(target);
                }
                evaluate(binary->getLHS());
                evaluate(binary->getRHS());
                return {};
            }
            if (binary->getOpcode() == clang::BO_Comma) {
                evaluate(binary->getLHS());
                return evaluate(binary->getRHS());
            }
            PointerValue lhs = evaluate(binary->getLHS());
            PointerValue rhs = evaluate(binary->getRHS());
            lhs.tokens.insert(rhs.tokens.begin(), rhs.tokens.end());
            return lhs;
        }

        if (const auto* conditional = llvm::dyn_cast<clang::ConditionalOperator>(expression)) {
            evaluate(conditional->getCond());
            PointerValue true_value = evaluate(conditional->getTrueExpr());
            PointerValue false_value = evaluate(conditional->getFalseExpr());
            true_value.tokens.insert(false_value.tokens.begin(), false_value.tokens.end());
            return true_value;
        }

        if (const auto* call = llvm::dyn_cast<clang::CallExpr>(expression)) {
            return evaluate_call(call);
        }

        if (const auto* delete_expr = llvm::dyn_cast<clang::CXXDeleteExpr>(expression)) {
            PointerValue value = evaluate(delete_expr->getArgument());
            diagnose_double_free(expression, value);
            state_.mark_freed(value, expression);
            return {};
        }

        for (const clang::Stmt* child : expression->children()) {
            if (const auto* child_expression = llvm::dyn_cast_or_null<clang::Expr>(child)) {
                evaluate(child_expression);
            }
        }
        return {};
    }

    PointerValue evaluate_call(const clang::CallExpr* call, bool diagnose_repeated_free = true) {
        propagate_memcpy_pointer_copy(call);

        if (is_deallocation_call(call) && call->getNumArgs() > 0) {
            PointerValue argument = evaluate(call->getArg(0));
            if (diagnose_repeated_free) {
                diagnose_double_free(call, argument);
            }
            state_.mark_freed(argument, call);
            for (unsigned index = 1; index < call->getNumArgs(); ++index) {
                evaluate(call->getArg(index));
            }
            return {};
        }

        if (call->getDirectCallee() == nullptr) {
            PointerValue callee = evaluate(call->getCallee());
            diagnose_uaf(call, callee);
        }

        for (const clang::Expr* argument : call->arguments()) {
            PointerValue value = evaluate(argument);
            diagnose_uaf(argument, value);
        }
        return returns_pointer(call) ? PointerValue{} : PointerValue{};
    }

    void assign(const clang::ValueDecl* target, const clang::Expr* rhs) {
        if (target == nullptr || !target->getType()->isPointerType()) {
            evaluate(rhs);
            return;
        }
        const auto* call = llvm::dyn_cast_or_null<clang::CallExpr>(ignore_pointer_casts(rhs));
        if (is_allocation_call(call)) {
            for (const clang::Expr* argument : call->arguments()) {
                evaluate(argument);
            }
            state_.set_fresh(target);
            return;
        }
        if (is_realloc_call(call)) {
            evaluate_call(call, false);
            state_.set_fresh(target);
            return;
        }
        if (llvm::isa_and_nonnull<clang::CXXNewExpr>(ignore_pointer_casts(rhs))) {
            state_.set_fresh(target);
            return;
        }
        PointerValue value = evaluate(rhs);
        if (value.tokens.empty()) {
            if (returns_pointer(ignore_pointer_casts(rhs))) {
                state_.set_fresh(target);
            } else {
                state_.kill(target);
            }
            return;
        }
        state_.set(target, std::move(value));
    }

    const clang::ValueDecl* address_of_lvalue(const clang::Expr* expression) {
        expression = strip_expr(expression);
        const auto* unary = llvm::dyn_cast_or_null<clang::UnaryOperator>(expression);
        if (unary == nullptr || unary->getOpcode() != clang::UO_AddrOf) {
            return nullptr;
        }
        return lvalue_declaration(unary->getSubExpr());
    }

    void propagate_memcpy_pointer_copy(const clang::CallExpr* call) {
        const clang::FunctionDecl* callee = call->getDirectCallee();
        if (callee == nullptr || callee->getName() != "memcpy" || call->getNumArgs() < 2) {
            return;
        }
        const clang::ValueDecl* destination = address_of_lvalue(call->getArg(0));
        const clang::ValueDecl* source = address_of_lvalue(call->getArg(1));
        if (destination == nullptr || source == nullptr ||
            !destination->getType()->isPointerType()) {
            return;
        }
        state_.set(destination, state_.value(source));
    }

    void diagnose_uaf(const clang::Stmt* statement, const PointerValue& value) {
        if (emitted_uaf_ == nullptr || value.tokens.empty() || !state_.maybe_freed(value) ||
            !emitted_uaf_->insert(statement).second) {
            return;
        }
        emit_uaf_warning(statement, state_.freed_note(value).value_or(statement));
    }

    void diagnose_double_free(const clang::Stmt* statement, const PointerValue& value) {
        if (emitted_double_free_ == nullptr || value.tokens.empty() || !state_.maybe_freed(value) ||
            !emitted_double_free_->insert(statement).second) {
            return;
        }
        emit_double_free_warning(statement, state_.freed_note(value).value_or(statement));
    }

    void emit_uaf_warning(const clang::Stmt* use_stmt, const clang::Stmt* free_stmt) {
        clang::DiagnosticsEngine& diagnostics = context_.getDiagnostics();
        const clang::DiagnosticsEngine::Level level = returnguard::options().fail_on_diagnostics
                                                          ? clang::DiagnosticsEngine::Error
                                                          : clang::DiagnosticsEngine::Warning;
        const unsigned diagnostic_id = diagnostics.getCustomDiagID(
            level, "returnguard safety: use of memory after it was freed");
        const unsigned note_id = diagnostics.getCustomDiagID(
            clang::DiagnosticsEngine::Note, "returnguard safety: pointer was freed here");

        diagnostics.Report(use_stmt->getBeginLoc(), diagnostic_id);
        diagnostics.Report(free_stmt->getBeginLoc(), note_id);
    }

    void emit_double_free_warning(const clang::Stmt* later_free, const clang::Stmt* earlier_free) {
        clang::DiagnosticsEngine& diagnostics = context_.getDiagnostics();
        const clang::DiagnosticsEngine::Level level = returnguard::options().fail_on_diagnostics
                                                          ? clang::DiagnosticsEngine::Error
                                                          : clang::DiagnosticsEngine::Warning;
        const unsigned diagnostic_id = diagnostics.getCustomDiagID(
            level, "returnguard safety: potential double free of pointer");
        const unsigned note_id =
            diagnostics.getCustomDiagID(clang::DiagnosticsEngine::Note,
                                        "returnguard safety: pointer was freed here previously");

        diagnostics.Report(later_free->getBeginLoc(), diagnostic_id);
        diagnostics.Report(earlier_free->getBeginLoc(), note_id);
    }

    LifetimeState& state_;
    clang::ASTContext& context_;
    std::unordered_set<const clang::Stmt*>* emitted_uaf_;
    std::unordered_set<const clang::Stmt*>* emitted_double_free_;
};

void transfer_lifetime_block(const clang::CFGBlock& block, LifetimeState& state,
                             clang::ASTContext& context,
                             std::unordered_set<const clang::Stmt*>* emitted_uaf,
                             std::unordered_set<const clang::Stmt*>* emitted_double_free) {
    LifetimeTransfer transfer(state, context, emitted_uaf, emitted_double_free);
    for (const clang::CFGElement& element : block) {
        if (const auto statement = element.getAs<clang::CFGStmt>()) {
            transfer.process(statement->getStmt());
        }
    }
}

void analyze_lifetime(const clang::FunctionDecl* function, clang::ASTContext& context) {
    if (function == nullptr || !function->doesThisDeclarationHaveABody()) {
        return;
    }

    clang::CFG::BuildOptions options;
    options.PruneTriviallyFalseEdges = true;
    options.AddInitializers = true;
    std::unique_ptr<clang::CFG> cfg =
        clang::CFG::buildCFG(function, function->getBody(), &context, options);
    if (!cfg) {
        return;
    }

    const unsigned block_count = cfg->getNumBlockIDs();
    std::vector<std::optional<LifetimeState>> inputs(block_count);
    std::deque<const clang::CFGBlock*> worklist;
    const clang::CFGBlock& entry = cfg->getEntry();
    LifetimeState entry_state;
    for (const clang::ParmVarDecl* parameter : function->parameters()) {
        if (parameter != nullptr && parameter->getType()->isPointerType()) {
            entry_state.set_fresh(parameter);
        }
    }
    inputs[entry.getBlockID()] = std::move(entry_state);
    worklist.push_back(&entry);

    while (!worklist.empty()) {
        const clang::CFGBlock* block = worklist.front();
        worklist.pop_front();
        LifetimeState output = *inputs[block->getBlockID()];
        transfer_lifetime_block(*block, output, context, nullptr, nullptr);

        for (const clang::CFGBlock* successor : block->succs()) {
            if (successor == nullptr) {
                continue;
            }
            std::optional<LifetimeState>& input = inputs[successor->getBlockID()];
            if (!input.has_value()) {
                input = output;
                worklist.push_back(successor);
                continue;
            }
            LifetimeState merged = *input;
            if (merged.join(output)) {
                input = std::move(merged);
                worklist.push_back(successor);
            }
        }
    }

    std::unordered_set<const clang::Stmt*> emitted_uaf;
    std::unordered_set<const clang::Stmt*> emitted_double_free;
    for (const clang::CFGBlock* block : *cfg) {
        if (block == nullptr || !inputs[block->getBlockID()].has_value()) {
            continue;
        }
        LifetimeState state = *inputs[block->getBlockID()];
        transfer_lifetime_block(*block, state, context, &emitted_uaf, &emitted_double_free);
    }
}

class SafetyVisitor : public clang::RecursiveASTVisitor<SafetyVisitor> {
  public:
    explicit SafetyVisitor(clang::ASTContext& context) : context_(context) {}

    bool VisitArraySubscriptExpr(clang::ArraySubscriptExpr* subscript) {
        maybe_emit_oob_for_array(subscript->getBase(), subscript->getIdx(), subscript);
        return true;
    }

    bool VisitUnaryOperator(clang::UnaryOperator* unary) {
        if (unary == nullptr)
            return true;

        // Handle pointer arithmetic patterns like "*(array + idx)"
        if (unary->getOpcode() == clang::UO_Deref) {
            const clang::Expr* sub = unary->getSubExpr()->IgnoreParenCasts();
            if (const auto* binary = llvm::dyn_cast<clang::BinaryOperator>(sub)) {
                if (binary->getOpcode() == clang::BO_Add || binary->getOpcode() == clang::BO_Sub) {
                    maybe_emit_oob_for_array(binary->getLHS(), binary->getRHS(), unary);
                    maybe_emit_oob_for_array(binary->getRHS(), binary->getLHS(), unary);
                }
            }
        }
        return true;
    }

    bool VisitBinaryOperator(clang::BinaryOperator* binary) {
        if (binary == nullptr)
            return true;

        // Detect obvious shift overflow: constant shift amount >= width of LHS
        if (binary->getOpcode() == clang::BO_Shl || binary->getOpcode() == clang::BO_Shr) {
            if (const clang::Expr* lhs = binary->getLHS()) {
                if (const clang::Expr* rhs = binary->getRHS()) {
                    auto rhs_val = evaluate_integer(rhs, context_);
                    if (rhs_val.has_value()) {
                        const unsigned long long shift_amount = rhs_val->getZExtValue();
                        const unsigned long long width = context_.getTypeSize(lhs->getType());
                        if (shift_amount >= width) {
                            emit_shift_overflow_warning(binary, shift_amount, width);
                        }
                        if (binary->getOpcode() == clang::BO_Shl &&
                            lhs->getType()->isSignedIntegerType() && shift_amount < width) {
                            auto lhs_val = evaluate_integer(lhs, context_);
                            if (lhs_val.has_value() && !lhs_val->isNegative()) {
                                const llvm::APInt shifted =
                                    lhs_val->extOrTrunc(static_cast<unsigned>(width))
                                        .shl(static_cast<unsigned>(shift_amount));
                                if (shifted.isNegative()) {
                                    emit_signed_shift_overflow_warning(binary);
                                }
                            }
                        }
                    }
                }
            }
        }

        return true;
    }

    bool VisitReturnStmt(clang::ReturnStmt* stmt) {
        const clang::Expr* val = stmt->getRetValue();
        if (val != nullptr && is_stack_address(val)) {
            emit_stack_return_warning(stmt, val);
        }
        return true;
    }

  private:
    bool is_stack_address(const clang::Expr* expression) {
        expression = expression->IgnoreParenCasts();
        if (expression == nullptr)
            return false;

        if (const auto* unary = llvm::dyn_cast<clang::UnaryOperator>(expression)) {
            if (unary->getOpcode() == clang::UO_AddrOf) {
                const clang::Expr* sub = unary->getSubExpr()->IgnoreParenCasts();
                if (const auto* ref = llvm::dyn_cast<clang::DeclRefExpr>(sub)) {
                    if (const auto* var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl())) {
                        return var->isLocalVarDecl() && !var->isStaticLocal();
                    }
                }
            }
        }

        if (const auto* ref = llvm::dyn_cast<clang::DeclRefExpr>(expression)) {
            if (const auto* var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl())) {
                if (var->getType()->isArrayType() && var->isLocalVarDecl() &&
                    !var->isStaticLocal()) {
                    return true;
                }
                if (var->isLocalVarDecl() && !var->isStaticLocal() && var->getInit() != nullptr) {
                    return is_stack_address(var->getInit());
                }
            }
        }

        return false;
    }

    void emit_oob_warning(const clang::ArraySubscriptExpr* expr, const llvm::APSInt& index,
                          const llvm::APInt& size) {
        clang::DiagnosticsEngine& diagnostics = context_.getDiagnostics();
        const clang::DiagnosticsEngine::Level level = returnguard::options().fail_on_diagnostics
                                                          ? clang::DiagnosticsEngine::Error
                                                          : clang::DiagnosticsEngine::Warning;
        const unsigned diagnostic_id = diagnostics.getCustomDiagID(
            level, "returnguard safety: array index %0 is out of bounds (array size is %1)");

        diagnostics.Report(expr->getIdx()->getExprLoc(), diagnostic_id)
            << llvm::toString(index, 10, index.isSigned()) << llvm::toString(size, 10, false);
    }

    void emit_stack_return_warning(const clang::ReturnStmt* stmt, const clang::Expr* val) {
        clang::DiagnosticsEngine& diagnostics = context_.getDiagnostics();
        const clang::DiagnosticsEngine::Level level = returnguard::options().fail_on_diagnostics
                                                          ? clang::DiagnosticsEngine::Error
                                                          : clang::DiagnosticsEngine::Warning;
        const unsigned diagnostic_id = diagnostics.getCustomDiagID(
            level, "returnguard safety: address of stack-allocated variable returned");

        diagnostics.Report(stmt->getReturnLoc(), diagnostic_id);
    }

    void emit_shift_overflow_warning(const clang::BinaryOperator* bin,
                                     unsigned long long shift_amount, unsigned long long width) {
        clang::DiagnosticsEngine& diagnostics = context_.getDiagnostics();
        const clang::DiagnosticsEngine::Level level = returnguard::options().fail_on_diagnostics
                                                          ? clang::DiagnosticsEngine::Error
                                                          : clang::DiagnosticsEngine::Warning;
        const unsigned diagnostic_id = diagnostics.getCustomDiagID(
            level, "returnguard safety: shift amount %0 is greater-or-equal to operand width %1");

        diagnostics.Report(bin->getOperatorLoc(), diagnostic_id)
            << std::to_string(shift_amount) << std::to_string(width);
    }

    void emit_signed_shift_overflow_warning(const clang::BinaryOperator* bin) {
        clang::DiagnosticsEngine& diagnostics = context_.getDiagnostics();
        const clang::DiagnosticsEngine::Level level = returnguard::options().fail_on_diagnostics
                                                          ? clang::DiagnosticsEngine::Error
                                                          : clang::DiagnosticsEngine::Warning;
        const unsigned diagnostic_id = diagnostics.getCustomDiagID(
            level, "returnguard safety: signed left shift result overflows operand type");

        diagnostics.Report(bin->getOperatorLoc(), diagnostic_id);
    }

    void emit_oob_warning_at(const clang::Expr* expr, const llvm::APSInt& index,
                             const llvm::APInt& size) {
        if (expr == nullptr)
            return;
        clang::DiagnosticsEngine& diagnostics = context_.getDiagnostics();
        const clang::DiagnosticsEngine::Level level = returnguard::options().fail_on_diagnostics
                                                          ? clang::DiagnosticsEngine::Error
                                                          : clang::DiagnosticsEngine::Warning;
        const unsigned diagnostic_id = diagnostics.getCustomDiagID(
            level, "returnguard safety: array index %0 is out of bounds (array size is %1)");

        diagnostics.Report(expr->getBeginLoc(), diagnostic_id)
            << llvm::toString(index, 10, index.isSigned()) << llvm::toString(size, 10, false);
    }

    void maybe_emit_oob_for_array(const clang::Expr* base_expr, const clang::Expr* idx_expr,
                                  const clang::Expr* report_site) {
        if (base_expr == nullptr || idx_expr == nullptr)
            return;
        base_expr = strip_expr(base_expr);
        idx_expr = strip_expr(idx_expr);

        // Resolve base declaration if present
        if (const auto* ref = llvm::dyn_cast<clang::DeclRefExpr>(base_expr)) {
            if (const auto* var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl())) {
                const clang::QualType t = var->getType();
                if (t->isConstantArrayType()) {
                    const auto* array_type =
                        llvm::dyn_cast<clang::ConstantArrayType>(t.getTypePtr());
                    if (!array_type)
                        return;
                    llvm::APInt size = array_type->getSize();
                    auto index_opt = evaluate_integer(idx_expr, context_);
                    if (index_opt.has_value()) {
                        if (index_opt->isNegative() ||
                            llvm::APSInt::compareValues(*index_opt, llvm::APSInt(size, true)) >=
                                0) {
                            emit_oob_warning_at(report_site, *index_opt, size);
                        }
                    }
                    return;
                }
            }
        }

        // Address-of to array: &arr + offset patterns
        if (const auto* unary = llvm::dyn_cast<clang::UnaryOperator>(base_expr)) {
            if (unary->getOpcode() == clang::UO_AddrOf) {
                const clang::Expr* sub = unary->getSubExpr()->IgnoreParenCasts();
                if (const auto* ref = llvm::dyn_cast<clang::DeclRefExpr>(sub)) {
                    if (const auto* var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl())) {
                        const clang::QualType t = var->getType();
                        if (t->isConstantArrayType()) {
                            const auto* array_type =
                                llvm::dyn_cast<clang::ConstantArrayType>(t.getTypePtr());
                            if (!array_type)
                                return;
                            llvm::APInt size = array_type->getSize();
                            auto index_opt = evaluate_integer(idx_expr, context_);
                            if (index_opt.has_value()) {
                                if (index_opt->isNegative() ||
                                    llvm::APSInt::compareValues(*index_opt,
                                                                llvm::APSInt(size, true)) >= 0) {
                                    emit_oob_warning_at(report_site, *index_opt, size);
                                }
                            }
                            return;
                        }
                    }
                }
            }
        }

        // If base expression itself carries a constant array type (e.g., array expression)
        if (base_expr->getType()->isConstantArrayType()) {
            const auto* array_type =
                llvm::dyn_cast<clang::ConstantArrayType>(base_expr->getType().getTypePtr());
            if (array_type) {
                llvm::APInt size = array_type->getSize();
                auto index_opt = evaluate_integer(idx_expr, context_);
                if (index_opt.has_value()) {
                    if (index_opt->isNegative() ||
                        llvm::APSInt::compareValues(*index_opt, llvm::APSInt(size, true)) >= 0) {
                        emit_oob_warning_at(report_site, *index_opt, size);
                    }
                }
                return;
            }
        }
    }

    clang::ASTContext& context_;
};

} // namespace

void Analyzer::analyze_safety(const clang::FunctionDecl* function) {
    if (function == nullptr || function->getBody() == nullptr) {
        return;
    }
    SafetyVisitor visitor(context_);
    visitor.TraverseStmt(const_cast<clang::Stmt*>(function->getBody()));
    analyze_lifetime(function, context_);
}

} // namespace returnguard::internal
