#include "Analyzer.hpp"

#include "NullStateAnalysis.hpp"

#include <returnguard/Options.hpp>

#include <clang/AST/ASTContext.h>
#include <clang/AST/Attr.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/Type.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/Specifiers.h>
#include <llvm/ADT/StringRef.h>

#include <memory>
#include <string>

namespace returnguard::internal {
namespace {

bool type_is_nullable(clang::QualType type) {
    if (type.isNull() || !type->isPointerType()) {
        return false;
    }

    const std::optional<clang::NullabilityKind> nullability =
        type->getNullability();
    return nullability.has_value() &&
           *nullability == clang::NullabilityKind::Nullable;
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

} // namespace

bool Analyzer::call_returns_nullable_pointer(const clang::CallExpr* call) const {
    if (call == nullptr) {
        return false;
    }

    const clang::QualType return_type = call->getCallReturnType(context_);
    if (!return_type->isPointerType()) {
        return false;
    }

    if (type_is_nullable(return_type)) {
        return true;
    }

    const clang::FunctionDecl* function = call->getDirectCallee();
    return function != nullptr && function_has_nullable_annotation(*function);
}

NullStateAnalysis* Analyzer::null_state_analysis(
    const clang::FunctionDecl* function) {
    if (function == nullptr || null_state_failures_.contains(function)) {
        return nullptr;
    }

    if (const auto iterator = null_state_cache_.find(function);
        iterator != null_state_cache_.end()) {
        return iterator->second.get();
    }

    std::unique_ptr<NullStateAnalysis> analysis =
        NullStateAnalysis::build(*function, context_);
    if (!analysis) {
        null_state_failures_.insert(function);
        return nullptr;
    }

    NullStateAnalysis* result = analysis.get();
    null_state_cache_.emplace(function, std::move(analysis));
    return result;
}

bool Analyzer::analyze_nullable_call(const clang::CallExpr* call) {
    if (!should_analyze_location(call->getExprLoc()) ||
        !call_returns_nullable_pointer(call)) {
        return false;
    }

    const clang::FunctionDecl* function = enclosing_function(call);
    NullStateAnalysis* analysis = null_state_analysis(function);
    if (analysis == nullptr) {
        return false;
    }

    const std::vector<const clang::Expr*> unsafe =
        analysis->unsafe_dereferences_for(*call);
    if (unsafe.empty()) {
        return false;
    }

    clang::DiagnosticsEngine& diagnostics = context_.getDiagnostics();
    const clang::DiagnosticsEngine::Level level =
        returnguard::options().fail_on_diagnostics
            ? clang::DiagnosticsEngine::Error
            : clang::DiagnosticsEngine::Warning;
    const unsigned diagnostic_id = diagnostics.getCustomDiagID(
        level,
        "returnguard: %0");
    const unsigned note_id = diagnostics.getCustomDiagID(
        clang::DiagnosticsEngine::Note,
        "returnguard: %0");

    const std::string message =
        "potentially-null return value of '" + function_name(call) +
        "' is dereferenced without a prior null check";

    clang::SourceLocation call_location =
        user_file_location(call->getExprLoc());
    if (call_location.isInvalid()) {
        call_location = call->getExprLoc();
    }

    for (const clang::Expr* dereference : unsafe) {
        clang::SourceLocation location =
            user_file_location(dereference->getExprLoc());
        if (location.isInvalid()) {
            location = dereference->getExprLoc();
        }

        diagnostics.Report(location, diagnostic_id) << message;
        diagnostics.Report(call_location, note_id)
            << "pointer returned here is marked nullable";
    }
    return true;
}

} // namespace returnguard::internal
