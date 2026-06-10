#pragma once

#include "Model.hpp"

namespace clang {
class ASTContext;
class Expr;
class IfStmt;
} // namespace clang

namespace returnguard::internal {
class Analyzer;

CheckResult analyze_direct_condition(const clang::Expr* condition, const clang::Expr* target,
                                     const Domain& domain, Analyzer& analyzer);

CheckResult analyze_direct_fallback_condition(const clang::Expr* condition,
                                              const clang::Expr* target, const Domain& domain,
                                              Analyzer& analyzer);

CheckResult analyze_direct_if(const clang::IfStmt* statement, const clang::Expr* target,
                              const Domain& domain, Analyzer& analyzer);
} // namespace returnguard::internal
