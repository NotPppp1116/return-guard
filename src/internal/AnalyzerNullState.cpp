#include "Analyzer.hpp"

#include "AstUtils.hpp"
#include "ContractPolicy.hpp"
#include "NullStateAnalysis.hpp"
#include "ReturnCollector.hpp"

#include <returnguard/Options.hpp>

#include <clang/AST/ASTContext.h>
#include <clang/AST/Attr.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Type.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/Specifiers.h>

#include <memory>
#include <string>
#include <unordered_set>

namespace returnguard::internal {
namespace {

bool type_is_nullable(clang::QualType type) {
    if (type.isNull() || !type->isPointerType()) {
        return false;
    }

    const std::optional<clang::NullabilityKind> nullability = type->getNullability();
    return nullability.has_value() && *nullability == clang::NullabilityKind::Nullable;
}

bool function_has_nullable_annotation(const clang::FunctionDecl& function) {
    for (const clang::FunctionDecl* redeclaration : function.redecls()) {
        if (type_is_nullable(redeclaration->getReturnType())) {
            return true;
        }

        for (const clang::AnnotateAttr* attribute :
             redeclaration->specific_attrs<clang::AnnotateAttr>()) {
            if (attribute->getAnnotation() == "returnguard.nullable") {
                return true;
            }
        }
    }
    return false;
}

bool function_returns_nullable_pointer_expression(const clang::Expr* expr,
                                                  clang::ASTContext& context,
                                                  const Analyzer& analyzer);

bool expression_contains_nonnull_factory(const clang::Expr* expr);

bool expression_can_return_null_literal(const clang::Expr* expr, clang::ASTContext& context) {
    if (expr == nullptr) {
        return false;
    }
    const clang::Expr* stripped = expr->IgnoreParenCasts();
    if (stripped->isNullPointerConstant(context, clang::Expr::NPC_ValueDependentIsNotNull) !=
        clang::Expr::NPCK_NotNull) {
        return true;
    }
    if (const auto* conditional = llvm::dyn_cast<clang::ConditionalOperator>(stripped)) {
        return expression_can_return_null_literal(conditional->getTrueExpr(), context) ||
               expression_can_return_null_literal(conditional->getFalseExpr(), context);
    }
    if (const auto* conditional = llvm::dyn_cast<clang::BinaryConditionalOperator>(stripped)) {
        return expression_can_return_null_literal(conditional->getCommon(), context) ||
               expression_can_return_null_literal(conditional->getFalseExpr(), context);
    }
    return false;
}

// NullableVariableVisitor was previously defined here but is unused.
// Removed to eliminate dead code; nullability checks use
// NullStateAnalysis and helper functions elsewhere in this file.

bool function_returns_nullable_pointer_expression(const clang::Expr* expr,
                                                  clang::ASTContext& context,
                                                  const Analyzer& analyzer) {
    if (expr == nullptr) {
        return false;
    }
    const clang::Expr* stripped = expr->IgnoreParenCasts();

    if (stripped->isNullPointerConstant(context, clang::Expr::NPC_ValueDependentIsNotNull) !=
        clang::Expr::NPCK_NotNull) {
        return true;
    }

    if (const auto* call = llvm::dyn_cast<clang::CallExpr>(stripped)) {
        if (analyzer.call_returns_nullable_pointer(call)) {
            return true;
        }
    }

    if (const auto* conditional = llvm::dyn_cast<clang::ConditionalOperator>(stripped)) {
        return function_returns_nullable_pointer_expression(conditional->getTrueExpr(), context,
                                                            analyzer) ||
               function_returns_nullable_pointer_expression(conditional->getFalseExpr(), context,
                                                            analyzer);
    }

    if (const auto* conditional = llvm::dyn_cast<clang::BinaryConditionalOperator>(stripped)) {
        return function_returns_nullable_pointer_expression(conditional->getCommon(), context,
                                                            analyzer) ||
               function_returns_nullable_pointer_expression(conditional->getFalseExpr(), context,
                                                            analyzer);
    }

    return false;
}

bool function_is_nonnull_owner_factory(const clang::FunctionDecl* function) {
    if (function == nullptr || function->getIdentifier() == nullptr) {
        return false;
    }
    const llvm::StringRef name = function->getName();
    return name == "makeUnique" || name == "makeShared" ||
           name == "make_unique" || name == "make_shared";
}

bool function_is_error_pointer_factory(const clang::FunctionDecl* function) {
    if (function == nullptr || function->getIdentifier() == nullptr) {
        return false;
    }
    const llvm::StringRef name = function->getName();
    return name == "ERR_PTR" || name == "ERR_CAST";
}

bool expression_returns_error_pointer(const clang::Expr* expr) {
    expr = strip_expr(expr);
    if (expr == nullptr) {
        return false;
    }

    if (const auto* call = llvm::dyn_cast<clang::CallExpr>(expr)) {
        return function_is_error_pointer_factory(call->getDirectCallee());
    }

    if (const auto* conditional = llvm::dyn_cast<clang::ConditionalOperator>(expr)) {
        return expression_returns_error_pointer(conditional->getTrueExpr()) ||
               expression_returns_error_pointer(conditional->getFalseExpr());
    }

    if (const auto* conditional = llvm::dyn_cast<clang::BinaryConditionalOperator>(expr)) {
        return expression_returns_error_pointer(conditional->getCommon()) ||
               expression_returns_error_pointer(conditional->getFalseExpr());
    }

    return false;
}

bool expression_contains_nonnull_factory(const clang::Expr* expr) {
    expr = strip_expr(expr);
    if (expr == nullptr) {
        return false;
    }

    if (const auto* call = llvm::dyn_cast<clang::CallExpr>(expr)) {
        if (function_is_nonnull_owner_factory(call->getDirectCallee())) {
            return true;
        }
    }

    for (const clang::Stmt* child : expr->children()) {
        if (const auto* child_expr = llvm::dyn_cast_or_null<clang::Expr>(child);
            child_expr != nullptr && expression_contains_nonnull_factory(child_expr)) {
            return true;
        }
    }
    return false;
}

bool expression_is_nonnull_owner(const clang::Expr* expr) {
    expr = strip_expr(expr);
    if (expr == nullptr) {
        return false;
    }

    if (const auto* call = llvm::dyn_cast<clang::CallExpr>(expr)) {
        if (function_is_nonnull_owner_factory(call->getDirectCallee())) {
            return true;
        }
    }

    const auto* member_call = llvm::dyn_cast<clang::CXXMemberCallExpr>(expr);
    if (member_call == nullptr) {
        return false;
    }

    const clang::CXXMethodDecl* method = member_call->getMethodDecl();
    if (!has_identifier_name(method, "emplace_back")) {
        return false;
    }

    for (const clang::Expr* argument : member_call->arguments()) {
        if (expression_contains_nonnull_factory(argument)) {
            return true;
        }
    }
    return false;
}

bool call_is_get_on_nonnull_owner(const clang::CallExpr* call) {
    const auto* member_call = llvm::dyn_cast_or_null<clang::CXXMemberCallExpr>(call);
    if (member_call == nullptr || !has_identifier_name(member_call->getMethodDecl(), "get")) {
        return false;
    }
    return expression_is_nonnull_owner(member_call->getImplicitObjectArgument());
}

} // namespace

bool Analyzer::call_returns_nullable_pointer(const clang::CallExpr* call) const {
    if (call == nullptr) {
        return false;
    }

    const clang::QualType return_type = call->getCallReturnType(context_);
    if (!return_type->isPointerType()) {
        return false;
    }

    if (call_is_get_on_nonnull_owner(call)) {
        return false;
    }

    if (type_is_nullable(return_type)) {
        return true;
    }

    const clang::FunctionDecl* function = call->getDirectCallee();
    if (function == nullptr) {
        return false;
    }

    if (failure_contract(*function, source_manager_) == FailurePredicate::Null) {
        return true;
    }

    return is_nullable_function(function);
}

bool Analyzer::is_nullable_function(const clang::FunctionDecl* function) const {
    return is_nullable_function_impl(function, active_nullable_checks_);
}

bool Analyzer::is_nullable_function_impl(
    const clang::FunctionDecl* function,
    std::unordered_set<const clang::FunctionDecl*>& active) const {
    const clang::FunctionDecl* canonical = function->getCanonicalDecl();

    if (const auto iterator = nullable_cache_.find(canonical); iterator != nullable_cache_.end()) {
        return iterator->second;
    }

    if (function_has_nullable_annotation(*canonical)) {
        nullable_cache_[canonical] = true;
        return true;
    }

    if (active.contains(canonical)) {
        return false;
    }
    active.insert(canonical);

    const clang::FunctionDecl* definition = nullptr;
    if (canonical->hasBody(definition) && definition != nullptr) {
        std::vector<const clang::ReturnStmt*> return_stmts;
        ReturnCollector collector(context_, return_stmts);
        collector.TraverseStmt(const_cast<clang::Stmt*>(definition->getBody()));

        bool has_error_pointer_return = false;
        bool has_null_literal_return = false;
        for (const clang::ReturnStmt* return_stmt : return_stmts) {
            const clang::Expr* value =
                return_stmt == nullptr ? nullptr : return_stmt->getRetValue();
            has_error_pointer_return =
                has_error_pointer_return || expression_returns_error_pointer(value);
            has_null_literal_return =
                has_null_literal_return || expression_can_return_null_literal(value, context_);
        }
        if (has_error_pointer_return && !has_null_literal_return) {
            active.erase(canonical);
            nullable_cache_[canonical] = false;
            return false;
        }

        NullStateAnalysis* analysis = null_state_analysis(definition);
        if (analysis != nullptr) {
            std::vector<NullSource> sources;

            // 1. Check parameters
            for (const clang::ParmVarDecl* param : definition->parameters()) {
                if (param != nullptr && type_is_nullable(param->getType())) {
                    sources.push_back({.decl = param});
                }
            }

            // 2. Check calls inside body
            class CallFinder : public clang::RecursiveASTVisitor<CallFinder> {
              public:
                CallFinder(const Analyzer& analyzer, std::vector<NullSource>& sources)
                    : analyzer_(analyzer), sources_(sources) {}

                bool VisitCallExpr(clang::CallExpr* call) {
                    if (analyzer_.call_returns_nullable_pointer(call)) {
                        sources_.push_back({.expr = call});
                    }
                    return true;
                }

              private:
                const Analyzer& analyzer_;
                std::vector<NullSource>& sources_;
            };

            CallFinder finder(*this, sources);
            finder.TraverseStmt(const_cast<clang::Stmt*>(definition->getBody()));

            // Helper lambda to check if a return expression is nullable
            std::function<bool(const clang::Expr*, const clang::ReturnStmt*)> is_expr_nullable =
                [&](const clang::Expr* expr, const clang::ReturnStmt* return_stmt) -> bool {
                if (expr == nullptr) {
                    return false;
                }
                const clang::Expr* stripped = expr->IgnoreParenCasts();

                if (stripped->isNullPointerConstant(context_,
                                                    clang::Expr::NPC_ValueDependentIsNotNull) !=
                    clang::Expr::NPCK_NotNull) {
                    return true;
                }

                if (expression_can_return_null_literal(stripped, context_)) {
                    return true;
                }

                if (const auto* call = llvm::dyn_cast<clang::CallExpr>(stripped)) {
                    if (this->call_returns_nullable_pointer(call)) {
                        return true;
                    }
                }

                for (const NullSource& source : sources) {
                    if (analysis->is_expression_nullable_at(source, return_stmt, stripped)) {
                        return true;
                    }
                }

                return false;
            };

            for (const clang::ReturnStmt* return_stmt : return_stmts) {
                if (return_stmt != nullptr && return_stmt->getRetValue() != nullptr) {
                    if (is_expr_nullable(return_stmt->getRetValue(), return_stmt)) {
                        active.erase(canonical);
                        nullable_cache_[canonical] = true;
                        return true;
                    }
                }
            }
        } else {
            // Fallback syntactic check
            for (const clang::ReturnStmt* return_stmt : return_stmts) {
                if (return_stmt != nullptr && return_stmt->getRetValue() != nullptr) {
                    if (function_returns_nullable_pointer_expression(return_stmt->getRetValue(),
                                                                     context_, *this)) {
                        active.erase(canonical);
                        nullable_cache_[canonical] = true;
                        return true;
                    }
                }
            }
        }
    }

    active.erase(canonical);
    nullable_cache_[canonical] = false;
    return false;
}

NullStateAnalysis* Analyzer::null_state_analysis(const clang::FunctionDecl* function) const {
    if (function == nullptr || null_state_failures_.contains(function)) {
        return nullptr;
    }

    if (const auto iterator = null_state_cache_.find(function);
        iterator != null_state_cache_.end()) {
        return iterator->second.get();
    }

    std::unique_ptr<NullStateAnalysis> analysis = NullStateAnalysis::build(*function, context_);
    if (!analysis) {
        null_state_failures_.insert(function);
        return nullptr;
    }

    NullStateAnalysis* result = analysis.get();
    null_state_cache_.emplace(function, std::move(analysis));
    return result;
}

void Analyzer::analyze_nullable_call(const clang::CallExpr* call) {
    if (!should_analyze_location(call->getExprLoc()) ||
        (!returnguard::options().include_operators && call_is_operator(call)) ||
        !call_returns_nullable_pointer(call)) {
        return;
    }

    const clang::FunctionDecl* function = enclosing_function(call);
    NullStateAnalysis* analysis = null_state_analysis(function);
    if (analysis == nullptr) {
        return;
    }

    const std::vector<const clang::Expr*> unsafe = analysis->unsafe_dereferences_for(*call);
    if (unsafe.empty()) {
        return;
    }

    clang::DiagnosticsEngine& diagnostics = context_.getDiagnostics();
    const clang::DiagnosticsEngine::Level level = returnguard::options().fail_on_diagnostics
                                                      ? clang::DiagnosticsEngine::Error
                                                      : clang::DiagnosticsEngine::Warning;
    const unsigned diagnostic_id = diagnostics.getCustomDiagID(level, "returnguard: %0");
    const unsigned note_id =
        diagnostics.getCustomDiagID(clang::DiagnosticsEngine::Note, "returnguard: %0");

    const std::string message = "potentially-null return value of '" + function_name(call) +
                                "' is dereferenced without a prior null check";

    clang::SourceLocation call_location = user_file_location(call->getExprLoc());
    if (call_location.isInvalid()) {
        call_location = call->getExprLoc();
    }

    for (const clang::Expr* dereference : unsafe) {
        clang::SourceLocation location = user_file_location(dereference->getExprLoc());
        if (location.isInvalid()) {
            location = dereference->getExprLoc();
        }

        diagnostics.Report(location, diagnostic_id) << message;
        diagnostics.Report(call_location, note_id) << "pointer returned here is marked nullable";
    }
}

} // namespace returnguard::internal
