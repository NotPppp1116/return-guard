#include "SafetyVisitor.hpp"

#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <llvm/Support/Casting.h>

namespace returnguard::internal {

bool SafetyVisitor::is_stack_address(
    const clang::Expr* expression) const {
    expression = expression->IgnoreParenCasts();
    if (expression == nullptr) {
        return false;
    }

    if (const auto* unary =
            llvm::dyn_cast<clang::UnaryOperator>(expression)) {
        if (unary->getOpcode() == clang::UO_AddrOf) {
            const clang::Expr* operand =
                unary->getSubExpr()->IgnoreParenCasts();
            if (const auto* reference =
                    llvm::dyn_cast<clang::DeclRefExpr>(operand)) {
                if (const auto* variable =
                        llvm::dyn_cast<clang::VarDecl>(
                            reference->getDecl())) {
                    return variable->isLocalVarDecl() &&
                           !variable->isStaticLocal();
                }
            }
        }
    }

    if (const auto* reference =
            llvm::dyn_cast<clang::DeclRefExpr>(expression)) {
        if (const auto* variable =
                llvm::dyn_cast<clang::VarDecl>(reference->getDecl())) {
            if (variable->getType()->isArrayType() &&
                variable->isLocalVarDecl() &&
                !variable->isStaticLocal()) {
                return true;
            }
            if (variable->isLocalVarDecl() &&
                !variable->isStaticLocal() &&
                variable->getInit() != nullptr) {
                return is_stack_address(variable->getInit());
            }
        }
    }
    return false;
}

} // namespace returnguard::internal
