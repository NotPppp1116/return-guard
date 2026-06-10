#include "SafetyVisitor.hpp"

#include "AstUtils.hpp"

#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <llvm/Support/Casting.h>

namespace returnguard::internal {

bool SafetyVisitor::statement_reassigns_variable(
    const clang::Stmt* statement,
    const clang::VarDecl* variable) const {
    if (const auto* binary =
            llvm::dyn_cast<clang::BinaryOperator>(statement)) {
        if (binary->isAssignmentOp() &&
            referenced_variable(binary->getLHS()) == variable) {
            return true;
        }
    }

    if (const auto* unary =
            llvm::dyn_cast<clang::UnaryOperator>(statement)) {
        if (unary->isIncrementDecrementOp() &&
            referenced_variable(unary->getSubExpr()) == variable) {
            return true;
        }
    }

    if (const auto* operator_call =
            llvm::dyn_cast<clang::CXXOperatorCallExpr>(statement)) {
        for (const clang::Expr* argument :
             operator_call->arguments()) {
            if (referenced_variable(argument) == variable) {
                return true;
            }
        }
    }

    if (const auto* member_call =
            llvm::dyn_cast<clang::CXXMemberCallExpr>(statement)) {
        for (const clang::Expr* argument : member_call->arguments()) {
            if (referenced_variable(argument) == variable) {
                return true;
            }
        }
    }

    if (const auto* call =
            llvm::dyn_cast<clang::CallExpr>(statement)) {
        for (const clang::Expr* argument : call->arguments()) {
            const clang::Expr* stripped =
                argument->IgnoreParenCasts();
            const auto* address =
                llvm::dyn_cast<clang::UnaryOperator>(stripped);
            if (address != nullptr &&
                address->getOpcode() == clang::UO_AddrOf &&
                referenced_variable(address->getSubExpr()) == variable) {
                return true;
            }
        }
    }
    return false;
}

bool SafetyVisitor::is_deallocation_of_variable(
    const clang::Stmt* statement,
    const clang::VarDecl* variable) const {
    if (const auto* call =
            llvm::dyn_cast_or_null<clang::CallExpr>(statement)) {
        const clang::FunctionDecl* callee = call->getDirectCallee();
        if (callee != nullptr && callee->getName() == "free" &&
            call->getNumArgs() > 0U) {
            return referenced_variable(call->getArg(0)) == variable;
        }
    }
    if (const auto* delete_expression =
            llvm::dyn_cast_or_null<clang::CXXDeleteExpr>(statement)) {
        return referenced_variable(delete_expression->getArgument()) ==
               variable;
    }
    return false;
}

} // namespace returnguard::internal
