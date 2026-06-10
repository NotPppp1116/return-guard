#pragma once

#include <llvm/ADT/APSInt.h>
#include <llvm/ADT/StringExtras.h>

#include <optional>

namespace clang {
class ASTContext;
class Expr;
class Stmt;
class VarDecl;
} // namespace clang

namespace returnguard::internal {

[[nodiscard]] const clang::Expr* strip_expr(const clang::Expr* expression);
[[nodiscard]] std::optional<llvm::APSInt> evaluate_integer(const clang::Expr* expression,
                                                           const clang::ASTContext& context);
[[nodiscard]] const clang::VarDecl* referenced_variable(const clang::Expr* expression);
[[nodiscard]] bool expression_forwards_variable(const clang::Expr* expression,
                                                const clang::VarDecl* variable);
[[nodiscard]] bool statement_exits(const clang::Stmt* statement);
[[nodiscard]] bool expression_references_variable(const clang::Stmt* statement,
                                                  const clang::VarDecl* variable);

} // namespace returnguard::internal
