#pragma once

#include "Model.hpp"

#include <clang/Analysis/CFG.h>

#include <memory>

namespace clang {
class ASTContext;
class CallExpr;
class FunctionDecl;
} // namespace clang

namespace returnguard::internal {

class CFGValueFlow final {
  public:
    static std::unique_ptr<CFGValueFlow> build(
        const clang::FunctionDecl& function,
        clang::ASTContext& context);

    [[nodiscard]] ExpressionSet aliases_for(const clang::CallExpr& call) const;

  private:
    CFGValueFlow(
        std::unique_ptr<clang::CFG> cfg,
        clang::ASTContext& context);

    std::unique_ptr<clang::CFG> cfg_;
    clang::ASTContext& context_;
};

} // namespace returnguard::internal
