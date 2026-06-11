#include "Analyzer.hpp"

#include "CFGValueFlow.hpp"
#include "Instrumentation.hpp"
#include "NullStateAnalysis.hpp"
#include "SiteMetadata.hpp"

#include <returnguard/Options.hpp>

#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Rewrite/Core/Rewriter.h>

#include <memory>
#include <vector>

namespace returnguard::internal {

Analyzer::Analyzer(clang::ASTContext& context,
                   clang::Rewriter* rewriter,
                   std::vector<SiteMetadata>* sites)
    : context_(context), source_manager_(context.getSourceManager()) {
    if (rewriter != nullptr && sites != nullptr) {
        instrumentation_ =
            std::make_unique<Instrumentation>(context, *rewriter, *sites);
    }
}

Analyzer::~Analyzer() = default;

bool Analyzer::shouldVisitTemplateInstantiations() const {
    return false;
}

bool Analyzer::shouldVisitImplicitCode() const {
    return false;
}

bool Analyzer::VisitCallExpr(clang::CallExpr* call) {
    analyze_nullable_call(call);

    bool instrumented = false;
    if (instrumentation_ != nullptr && call != nullptr &&
        should_analyze_location(call->getExprLoc())) {
        const clang::QualType return_type = call->getCallReturnType(context_);
        if (!return_type->isVoidType() && !return_type->isDependentType() &&
            (returnguard::options().include_reference_returns ||
             !return_type->isReferenceType())) {
            const Domain domain = call_domain(call);
            const CheckResult handling = classify_call(call, domain);
            instrumented = instrumentation_->consider(call, handling);
        }
    }

    if (!instrumented) {
        analyze_call(call);
    }
    return true;
}

bool Analyzer::VisitFunctionDecl(clang::FunctionDecl* function) {
    if (function != nullptr && function->hasBody()) {
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
