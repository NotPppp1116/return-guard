#pragma once

#include "Model.hpp"

#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/SourceLocation.h>

#include <vector>

namespace clang {
class BinaryOperator;
class ConditionalOperator;
class DeclRefExpr;
class DeclStmt;
class DoStmt;
class Expr;
class ForStmt;
class IfStmt;
class ReturnStmt;
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
    bool VisitCallExpr(clang::CallExpr* call);
    bool VisitIfStmt(clang::IfStmt* statement);
    bool VisitWhileStmt(clang::WhileStmt* statement);
    bool VisitDoStmt(clang::DoStmt* statement);
    bool VisitForStmt(clang::ForStmt* statement);
    bool TraverseDeclStmt(clang::DeclStmt* statement, DataRecursionQueue* queue = nullptr);
    bool TraverseConditionalOperator(clang::ConditionalOperator* expression);
    bool TraverseReturnStmt(clang::ReturnStmt* statement);
    bool VisitBinaryOperator(clang::BinaryOperator* expression);
    bool VisitUnaryOperator(clang::UnaryOperator* expression);
    bool VisitDeclRefExpr(clang::DeclRefExpr* reference);

    [[nodiscard]] bool has_any_use() const;
    [[nodiscard]] bool has_any_check() const;
    [[nodiscard]] bool forwarded() const;
    [[nodiscard]] bool exhaustive() const;
    [[nodiscard]] const std::vector<bool>& covered() const;

  private:
    [[nodiscard]] bool occurs_after(clang::SourceLocation location) const;
    [[nodiscard]] bool tracks(const clang::VarDecl* variable) const;
    [[nodiscard]] const clang::VarDecl* tracked_reference(const clang::Expr* expression) const;
    [[nodiscard]] const clang::VarDecl*
    forwarded_tracked_variable(const clang::Expr* expression) const;
    [[nodiscard]] const clang::VarDecl* condition_variable(const clang::Stmt* statement) const;
    [[nodiscard]] bool expression_references_tracked(const clang::Stmt* statement) const;
    void add_tracked_variable(const clang::VarDecl* variable);
    void remove_tracked_variable(const clang::VarDecl* variable);
    void mark_condition(const clang::Expr* condition, const clang::VarDecl* variable);
    void mark_if_chain(const clang::IfStmt* statement, const clang::VarDecl* variable);
    void mark_switch(const clang::SwitchStmt* statement);

    Analyzer& analyzer_;
    clang::SourceLocation after_;
    const Domain& domain_;
    std::vector<const clang::VarDecl*> tracked_variables_;
    bool has_any_use_ = false;
    bool has_any_check_ = false;
    bool forwarded_ = false;
    bool exhaustive_ = false;
    bool invalidated_ = false;
    std::vector<bool> covered_;
};

} // namespace returnguard::internal
