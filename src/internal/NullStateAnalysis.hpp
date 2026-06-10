#pragma once

#include <clang/Analysis/CFG.h>

#include <memory>
#include <vector>

namespace clang {
class ASTContext;
class CallExpr;
class Expr;
class FunctionDecl;
} // namespace clang

namespace returnguard::internal {

struct NullSource {
    const clang::Expr* expr = nullptr;
    const clang::ValueDecl* decl = nullptr;
};

class NullStateAnalysis final {
  public:
    static std::unique_ptr<NullStateAnalysis> build(const clang::FunctionDecl& function,
                                                    clang::ASTContext& context);

    [[nodiscard]] std::vector<const clang::Expr*>
    unsafe_dereferences_for(const clang::CallExpr& call) const;

    [[nodiscard]] bool is_source_nullable_at(const NullSource& source, const clang::Stmt* stmt,
                                             const clang::ValueDecl* var) const;

    [[nodiscard]] bool is_expression_nullable_at(const NullSource& source, const clang::Stmt* stmt,
                                                 const clang::Expr* expression) const;

  private:
    NullStateAnalysis(std::unique_ptr<clang::CFG> cfg, clang::ASTContext& context);

    std::unique_ptr<clang::CFG> cfg_;
    clang::ASTContext& context_;
};

} // namespace returnguard::internal
