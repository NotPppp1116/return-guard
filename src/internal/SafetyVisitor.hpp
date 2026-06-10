#pragma once

#include <clang/AST/RecursiveASTVisitor.h>

#include <memory>

namespace clang {
class ASTContext;
class ArraySubscriptExpr;
class BinaryOperator;
class CFG;
class CXXDeleteExpr;
class Expr;
class FunctionDecl;
class ReturnStmt;
class Stmt;
class UnaryOperator;
class VarDecl;
} // namespace clang

namespace llvm {
class APInt;
class APSInt;
} // namespace llvm

namespace returnguard::internal {

class SafetyVisitor final : public clang::RecursiveASTVisitor<SafetyVisitor> {
  public:
    SafetyVisitor(
        const clang::FunctionDecl* function,
        clang::ASTContext& context);

    bool VisitArraySubscriptExpr(clang::ArraySubscriptExpr* subscript);
    bool VisitUnaryOperator(clang::UnaryOperator* unary);
    bool VisitBinaryOperator(clang::BinaryOperator* binary);
    bool VisitReturnStmt(clang::ReturnStmt* statement);
    bool VisitCallExpr(clang::CallExpr* call);
    bool VisitCXXDeleteExpr(clang::CXXDeleteExpr* expression);

  private:
    [[nodiscard]] bool is_stack_address(const clang::Expr* expression) const;
    void check_uaf_for_variable(
        const clang::VarDecl* variable,
        const clang::Stmt* deallocation);
    [[nodiscard]] bool statement_reassigns_variable(
        const clang::Stmt* statement,
        const clang::VarDecl* variable) const;
    [[nodiscard]] bool is_deallocation_of_variable(
        const clang::Stmt* statement,
        const clang::VarDecl* variable) const;

    void emit_stack_return_warning(const clang::ReturnStmt* statement) const;
    void emit_uaf_warning(
        const clang::Stmt* use,
        const clang::Stmt* deallocation) const;
    void emit_double_free_warning(
        const clang::Stmt* later,
        const clang::Stmt* earlier) const;
    void emit_shift_overflow_warning(
        const clang::BinaryOperator* binary,
        unsigned long long shift_amount,
        unsigned long long width) const;
    void emit_signed_shift_overflow_warning(
        const clang::BinaryOperator* binary) const;
    void emit_oob_warning_at(
        const clang::Expr* expression,
        const llvm::APSInt& index,
        const llvm::APInt& size) const;
    void maybe_emit_oob_for_array(
        const clang::Expr* base_expression,
        const clang::Expr* index_expression,
        const clang::Expr* report_site) const;

    [[nodiscard]] unsigned diagnostic_level() const;

    const clang::FunctionDecl* function_;
    clang::ASTContext& context_;
    std::unique_ptr<clang::CFG> cfg_;
};

} // namespace returnguard::internal
