#pragma once

#include <clang/AST/RecursiveASTVisitor.h>

#include <vector>

namespace clang {
class ASTContext;
class Expr;
class LambdaExpr;
class ReturnStmt;
}

namespace returnguard::internal {

class ReturnCollector final : public clang::RecursiveASTVisitor<ReturnCollector> {
public:
    ReturnCollector(
        const clang::ASTContext& context,
        std::vector<const clang::Expr*>& expressions);

    bool VisitReturnStmt(clang::ReturnStmt* statement);
    bool TraverseLambdaExpr(clang::LambdaExpr* expression);

private:
    const clang::ASTContext& context_;
    std::vector<const clang::Expr*>& expressions_;
};

} // namespace returnguard::internal
