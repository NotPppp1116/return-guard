#pragma once

#include "Model.hpp"

#include <clang/AST/OperationKinds.h>
#include <llvm/ADT/APSInt.h>

namespace clang {
class Expr;
class VarDecl;
} // namespace clang

namespace returnguard::internal {

class Analyzer;

[[nodiscard]] Truth invert(Truth value);
[[nodiscard]] Truth logical_and(Truth lhs, Truth rhs);
[[nodiscard]] Truth logical_or(Truth lhs, Truth rhs);
[[nodiscard]] Truth compare_values(clang::BinaryOperatorKind opcode, const llvm::APSInt& lhs,
                                   const llvm::APSInt& rhs);
[[nodiscard]] SymbolicInteger symbolic_integer(const clang::Expr* expression,
                                               const clang::VarDecl* target, Analyzer& analyzer);
[[nodiscard]] SymbolicInteger symbolic_integer(const clang::Expr* expression,
                                               const clang::Expr* target, Analyzer& analyzer);
[[nodiscard]] Truth evaluate_condition_for_value(const clang::Expr* expression,
                                                 const clang::VarDecl* target,
                                                 const llvm::APSInt& target_value,
                                                 Analyzer& analyzer);
[[nodiscard]] Truth evaluate_condition_for_value(const clang::Expr* expression,
                                                 const clang::Expr* target,
                                                 const llvm::APSInt& target_value,
                                                 Analyzer& analyzer);
[[nodiscard]] Truth evaluate_condition_for_value(const clang::Expr* expression,
                                                 const ExpressionSet& targets,
                                                 const llvm::APSInt& target_value,
                                                 Analyzer& analyzer);
[[nodiscard]] bool is_guard_condition(const clang::Expr* expression, const clang::VarDecl* target,
                                      Analyzer& analyzer);
[[nodiscard]] bool is_guard_condition(const clang::Expr* expression, const clang::Expr* target,
                                      Analyzer& analyzer);
[[nodiscard]] bool is_guard_condition(const clang::Expr* expression, const ExpressionSet& targets,
                                      Analyzer& analyzer);

} // namespace returnguard::internal
