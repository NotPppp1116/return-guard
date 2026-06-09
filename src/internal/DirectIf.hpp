#pragma once

#include "Model.hpp"

namespace clang {
class ASTContext;
class Expr;
class IfStmt;
} // namespace clang

namespace returnguard::internal {
CheckResult analyze_direct_condition(const clang::Expr* condition, const clang::Expr* target,
                                     const Domain& domain, const clang::ASTContext& context);

CheckResult analyze_direct_fallback_condition(const clang::Expr* condition,
                                              const clang::Expr* target);

CheckResult analyze_direct_if(const clang::IfStmt* statement, const clang::Expr* target,
                              const Domain& domain, const clang::ASTContext& context);
} // namespace returnguard::internal
