#include "ReturnCollector.hpp"

#include <clang/AST/Stmt.h>

namespace returnguard::internal {

ReturnCollector::ReturnCollector(
    std::vector<const clang::Expr*>& expressions)
    : expressions_(expressions) {}

bool ReturnCollector::shouldVisitLambdaBody() const {
    return false;
}

bool ReturnCollector::VisitReturnStmt(clang::ReturnStmt* statement) {
    if (statement->getRetValue() != nullptr) {
        expressions_.push_back(statement->getRetValue());
    }
    return true;
}

} // namespace returnguard::internal
