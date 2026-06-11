#pragma once

#include <clang/Analysis/CFG.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>

#include <memory>
#include <vector>
#include <unordered_set>

namespace returnguard {
class Analyzer;
namespace internal {

class PointerStateAnalysis final {
  public:
    static std::unique_ptr<PointerStateAnalysis> build(const clang::FunctionDecl& function,
                                                       clang::ASTContext& context,
                                                       const returnguard::internal::Analyzer& analyzer);

    void diagnose(std::unordered_set<const clang::Stmt*>& emitted_uaf,
                  std::unordered_set<const clang::Stmt*>& emitted_double_free,
                  std::unordered_set<const clang::Stmt*>& emitted_null_deref);

  private:
    PointerStateAnalysis(std::unique_ptr<clang::CFG> cfg, clang::ASTContext& context, const returnguard::internal::Analyzer& analyzer);

    std::unique_ptr<clang::CFG> cfg_;
    clang::ASTContext& context_;
    const returnguard::internal::Analyzer& analyzer_;
};

} // namespace internal
} // namespace returnguard
