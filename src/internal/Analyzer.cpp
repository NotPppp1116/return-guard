#include "Analyzer.hpp"

#include "CFGValueFlow.hpp"
#include "NullStateAnalysis.hpp"

#include <returnguard/Options.hpp>

#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <clang/Basic/SourceManager.h>

#include <memory>

namespace returnguard::internal {

Analyzer::Analyzer(clang::ASTContext& context)
    : context_(context), source_manager_(context.getSourceManager()) {}

Analyzer::~Analyzer() = default;

bool Analyzer::shouldVisitTemplateInstantiations() const {
    return false;
}

bool Analyzer::shouldVisitImplicitCode() const {
    return false;
}

bool Analyzer::VisitCallExpr(clang::CallExpr* call) {
    analyze_nullable_call(call);
    analyze_call(call);
    return true;
}

bool Analyzer::VisitFunctionDecl(clang::FunctionDecl* function) {
    if (function != nullptr && function->hasBody() &&
        should_analyze_location(function->getLocation())) {
        analyze_safety(function);
    }
    return true;
}

const clang::ASTContext& Analyzer::context() const {
    return context_;
}

const clang::SourceManager& Analyzer::source_manager() const {
    return source_manager_;
}

} // namespace returnguard::internal
