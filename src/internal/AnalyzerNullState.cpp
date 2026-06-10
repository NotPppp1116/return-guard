#include "Analyzer.hpp"

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

#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace returnguard::internal {
namespace {

std::optional<clang::NullabilityKind> pointer_nullability(clang::QualType type) {
    if (type.isNull() || !type->isPointerType()) {
        return std::nullopt;
    }
    return type->getNullability();
}

bool type_is_nullable(clang::QualType type) {
    const std::optional<clang::NullabilityKind> nullability = pointer_nullability(type);
    return nullability.has_value() && *nullability == clang::NullabilityKind::Nullable;
}

bool type_is_nonnull(clang::QualType type) {
    const std::optional<clang::NullabilityKind> nullability = pointer_nullability(type);
    return nullability.has_value() && *nullability == clang::NullabilityKind::NonNull;
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

bool function_has_nonnull_contract(const clang::FunctionDecl& function) {
    for (const clang::FunctionDecl* redeclaration : function.redecls()) {
        if (type_is_nonnull(redeclaration->getReturnType()) ||
            redeclaration->hasAttr<clang::ReturnsNonNullAttr>()) {
            return true;
        }
    }
    return false;
}

bool function_returns_nullable_pointer_expression(const clang::Expr* expression,
                                                   clang::ASTContext& context,
                                                   const Analyzer& analyzer) {
    if (expression == nullptr) {
        return false;
    }
    const clang::Expr* stripped = expression->IgnoreParenCasts();

    if (stripped->isNullPointerConstant(context, clang::Expr::NPC_ValueDependentIsNotNull) !=
        clang::Expr::NPCK_NotNull) {
        return true;
    }

    if (const auto* call = llvm::dyn_cast<clang::CallExpr>(stripped)) {
        return analyzer.call_returns_nullable_pointer(call);
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

} // namespace

bool Analyzer::call_returns_nullable_pointer(const clang::CallExpr* call) const {
    if (call == nullptr) {
        return false;
    }

    const clang::QualType return_type = call->getCallReturnType(context_);
    if (!return_type->isPointerType()) {
        return false;
    }

    if (type_is_nonnull(return_type)) {
        return false;
    }
    if (type_is_nullable(return_type)) {
        return true;
    }

    const clang::FunctionDecl* function = call->getDirectCallee();
    if (function == nullptr) {
        // An indirect call with no nonnull type contract is not proven safe.
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
    if (function == nullptr || !function->getReturnType()->isPointerType()) {
        return false;
    }

    const clang::FunctionDecl* canonical = function->getCanonicalDecl();

    if (const auto iterator = nullable_cache_.find(canonical); iterator != nullable_cache_.end()) {
        return iterator->second;
    }

    if (function_has_nonnull_contract(*canonical)) {
        nullable_cache_[canonical] = false;
        return false;
    }
    if (function_has_nullable_annotation(*canonical)) {
        nullable_cache_[canonical] = true;
        return true;
    }

    const clang::FunctionDecl* definition = nullptr;
    if (!canonical->hasBody(definition) || definition == nullptr) {
        // For an opaque pointer-returning API, absence of a nonnull contract is
        // not evidence that dereferencing the result is safe.
        nullable_cache_[canonical] = true;
        return true;
    }

    if (active.contains(canonical)) {
        return false;
    }
    active.insert(canonical);

    std::vector<const clang::ReturnStmt*> return_statements;
    ReturnCollector collector(context_, return_statements);
    collector.TraverseStmt(const_cast<clang::Stmt*>(definition->getBody()));

    NullStateAnalysis* analysis = null_state_analysis(definition);
    if (analysis != nullptr) {
        std::vector<NullSource> sources;

        for (const clang::ParmVarDecl* parameter : definition->parameters()) {
            if (parameter != nullptr && type_is_nullable(parameter->getType())) {
                sources.push_back({.decl = parameter});
            }
        }

        class CallFinder final : public clang::RecursiveASTVisitor<CallFinder> {
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

        std::function<bool(const clang::Expr*, const clang::ReturnStmt*)> is_expression_nullable =
            [&](const clang::Expr* expression, const clang::ReturnStmt* return_statement) -> bool {
            if (expression == nullptr) {
                return false;
            }
            const clang::Expr* stripped = expression->IgnoreParenCasts();

            if (stripped->isNullPointerConstant(context_,
                                                clang::Expr::NPC_ValueDependentIsNotNull) !=
                clang::Expr::NPCK_NotNull) {
                return true;
            }

            if (const auto* call = llvm::dyn_cast<clang::CallExpr>(stripped)) {
                return call_returns_nullable_pointer(call);
            }

            if (const auto* conditional = llvm::dyn_cast<clang::ConditionalOperator>(stripped)) {
                return is_expression_nullable(conditional->getTrueExpr(), return_statement) ||
                       is_expression_nullable(conditional->getFalseExpr(), return_statement);
            }

            if (const auto* conditional =
                    llvm::dyn_cast<clang::BinaryConditionalOperator>(stripped)) {
                return is_expression_nullable(conditional->getCommon(), return_statement) ||
                       is_expression_nullable(conditional->getFalseExpr(), return_statement);
            }

            if (const auto* reference = llvm::dyn_cast<clang::DeclRefExpr>(stripped)) {
                if (const auto* variable = llvm::dyn_cast<clang::VarDecl>(reference->getDecl())) {
                    for (const NullSource& source : sources) {
                        if (analysis->is_source_nullable_at(source, return_statement, variable)) {
                            return true;
                        }
                    }
                }
            }

            return false;
        };

        for (const clang::ReturnStmt* return_statement : return_statements) {
            if (return_statement != nullptr && return_statement->getRetValue() != nullptr &&
                is_expression_nullable(return_statement->getRetValue(), return_statement)) {
                active.erase(canonical);
                nullable_cache_[canonical] = true;
                return true;
            }
        }
    } else {
        for (const clang::ReturnStmt* return_statement : return_statements) {
            if (return_statement != nullptr && return_statement->getRetValue() != nullptr &&
                function_returns_nullable_pointer_expression(return_statement->getRetValue(),
                                                             context_, *this)) {
                active.erase(canonical);
                nullable_cache_[canonical] = true;
                return true;
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
    if (!should_analyze_location(call->getExprLoc()) || !call_returns_nullable_pointer(call)) {
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
        diagnostics.Report(call_location, note_id)
            << "pointer returned here is nullable or has no nonnull contract";
    }
}

} // namespace returnguard::internal
