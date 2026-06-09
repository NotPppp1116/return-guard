#pragma once

#include "Model.hpp"

#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/SourceLocation.h>

#include <vector>

namespace clang {
class BinaryOperator;
class DeclRefExpr;
class DoStmt;
class Expr;
class ForStmt;
class IfStmt;
class SwitchStmt;
class UnaryOperator;
class VarDecl;
class WhileStmt;
} // namespace clang

namespace returnguard::internal {

class Analyzer;

class HandlerFinder final : public clang::RecursiveASTVisitor<HandlerFinder> {
  public:
    HandlerFinder(Analyzer& analyzer, const clang::VarDecl* variable, clang::SourceLocation after,
                  const Domain& domain);

    bool VisitSwitchStmt(clang::SwitchStmt* statement);
    bool VisitIfStmt(clang::IfStmt* statement);
    bool VisitWhileStmt(clang::WhileStmt* statement);
    bool VisitDoStmt(clang::DoStmt* statement);
    bool VisitForStmt(clang::ForStmt* statement);
    bool VisitBinaryOperator(clang::BinaryOperator* expression);
    bool VisitUnaryOperator(clang::UnaryOperator* expression);
    bool VisitDeclRefExpr(clang::DeclRefExpr* reference);

    [[nodiscard]] bool has_any_use() const;
    [[nodiscard]] bool has_any_check() const;
    [[nodiscard]] bool exhaustive() const;
    [[nodiscard]] const std::vector<bool>& covered() const;

  private:
    [[nodiscard]] bool occurs_after(clang::SourceLocation location) const;
    void mark_condition(const clang::Expr* condition);
    void mark_if_chain(const clang::IfStmt* statement);
    void mark_switch(const clang::SwitchStmt* statement);

    Analyzer& analyzer_;
    const clang::VarDecl* variable_;
    clang::SourceLocation after_;
    const Domain& domain_;
    bool has_any_use_ = false;
    bool has_any_check_ = false;
    bool exhaustive_ = false;
    bool invalidated_ = false;
    std::vector<bool> covered_;
};

} // namespace returnguard::internal
