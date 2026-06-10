#pragma once

#include <clang/AST/RecursiveASTVisitor.h>
#include <vector>

namespace clang {
class ASTContext;
class Expr;
class ReturnStmt;
}

namespace returnguard::internal {

class ReturnCollector final : public clang::RecursiveASTVisitor<ReturnCollector> {
public:
    ReturnCollector(
        const clang::ASTContext& context,
        std::vector<const clang::ReturnStmt*>& statements);

    bool shouldVisitLambdaBody() const;
    bool VisitReturnStmt(clang::ReturnStmt* statement);

private:
    std::vector<const clang::ReturnStmt*>& statements_;
};


} // namespace returnguard::internal
