#include "Analyzer.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <clang/Basic/SourceManager.h>

namespace returnguard::internal {

Analyzer::Analyzer(clang::ASTContext& context)
    : context_(context), source_manager_(context.getSourceManager()) {}

bool Analyzer::shouldVisitTemplateInstantiations() const {
    return false;
}

bool Analyzer::shouldVisitImplicitCode() const {
    return false;
}

bool Analyzer::VisitCallExpr(clang::CallExpr* call) {
    analyze_call(call);
    return true;
}

const clang::ASTContext& Analyzer::context() const {
    return context_;
}

const clang::SourceManager& Analyzer::source_manager() const {
    return source_manager_;
}

} // namespace returnguard::internal
