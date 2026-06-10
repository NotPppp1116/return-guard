#include "ReturnCollector.hpp"

#include <clang/AST/Stmt.h>

namespace returnguard::internal {

ReturnCollector::ReturnCollector(
    const clang::ASTContext&,
    std::vector<const clang::ReturnStmt*>& statements)
    : statements_(statements) {}

bool ReturnCollector::shouldVisitLambdaBody() const {
    return false;
}

bool ReturnCollector::VisitReturnStmt(clang::ReturnStmt* statement) {
    if (statement != nullptr) {
        statements_.push_back(statement);
    }
    return true;
}


} // namespace returnguard::internal
