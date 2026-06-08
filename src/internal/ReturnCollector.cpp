#include "ReturnCollector.hpp"

#include <clang/AST/ExprCXX.h>
#include <clang/AST/Stmt.h>

namespace returnguard::internal {

ReturnCollector::ReturnCollector(
    const clang::ASTContext& context,
    std::vector<const clang::Expr*>& expressions)
    : context_(context), expressions_(expressions) {}

bool ReturnCollector::VisitReturnStmt(clang::ReturnStmt* statement) {
    if (statement->getRetValue() != nullptr) {
        expressions_.push_back(statement->getRetValue());
    }
    return true;
}

bool ReturnCollector::TraverseLambdaExpr(clang::LambdaExpr*) {
    return true;
}

} // namespace returnguard::internal
