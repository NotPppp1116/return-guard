#pragma once

#include "Model.hpp"

namespace clang {
class ASTContext;
class Expr;
class IfStmt;
}

namespace returnguard::internal {
CheckResult analyze_direct_if(
    const clang::IfStmt* statement,
    const clang::Expr* target,
    const Domain& domain,
    const clang::ASTContext& context);
}
