#pragma once

#include <clang/AST/RecursiveASTVisitor.h>
#include <vector>

namespace clang {
class Expr;
class ReturnStmt;
}

namespace returnguard::internal {

class ReturnCollector final : public clang::RecursiveASTVisitor<ReturnCollector> {
public:
    explicit ReturnCollector(std::vector<const clang::Expr*>& expressions);

    bool shouldVisitLambdaBody() const;
    bool VisitReturnStmt(clang::ReturnStmt* statement);

private:
    std::vector<const clang::Expr*>& expressions_;
};

} // namespace returnguard::internal
