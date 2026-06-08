#include "Analyzer.hpp"

#include "AstUtils.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/ParentMapContext.h>
#include <clang/AST/Stmt.h>
#include <llvm/Support/Casting.h>

namespace returnguard::internal {
namespace {

bool is_transparent_wrapper(const clang::Expr* expression) {
    return llvm::isa<
        clang::ParenExpr,
        clang::ImplicitCastExpr,
        clang::CStyleCastExpr,
        clang::CXXStaticCastExpr,
        clang::CXXFunctionalCastExpr,
        clang::ExprWithCleanups,
        clang::MaterializeTemporaryExpr,
        clang::CXXBindTemporaryExpr>(expression);
}

bool carries_if_condition(const clang::Expr* expression) {
    if (is_transparent_wrapper(expression)) {
        return true;
    }

    if (const auto* unary = llvm::dyn_cast<clang::UnaryOperator>(expression)) {
        return unary->getOpcode() == clang::UO_LNot;
    }

    if (const auto* binary = llvm::dyn_cast<clang::BinaryOperator>(expression)) {
        return binary->isComparisonOp() ||
               binary->getOpcode() == clang::BO_LAnd ||
               binary->getOpcode() == clang::BO_LOr;
    }

    return false;
}

const clang::Expr* expression_from(const clang::DynTypedNode& node) {
    return node.get<clang::Expr>();
}

} // namespace

const clang::VarDecl* Analyzer::variable_initialized_by_call(
    const clang::CallExpr* call) const {
    clang::DynTypedNode current = clang::DynTypedNode::create(*call);
    for (unsigned depth = 0; depth < 24U; ++depth) {
        const auto parents = context_.getParents(current);
        if (parents.empty()) {
            return nullptr;
        }
        const clang::DynTypedNode& parent = parents[0];

        if (const auto* variable = parent.get<clang::VarDecl>()) {
            return variable->getInit() == expression_from(current)
                       ? variable
                       : nullptr;
        }

        const clang::Expr* expression = expression_from(parent);
        if (expression == nullptr || !is_transparent_wrapper(expression)) {
            return nullptr;
        }
        current = parent;
    }
    return nullptr;
}

const clang::VarDecl* Analyzer::variable_assigned_from_call(
    const clang::CallExpr* call) const {
    clang::DynTypedNode current = clang::DynTypedNode::create(*call);
    for (unsigned depth = 0; depth < 24U; ++depth) {
        const auto parents = context_.getParents(current);
        if (parents.empty()) {
            return nullptr;
        }
        const clang::DynTypedNode& parent = parents[0];

        if (const auto* binary = parent.get<clang::BinaryOperator>()) {
            if (!binary->isAssignmentOp() ||
                binary->getRHS() != expression_from(current)) {
                return nullptr;
            }
            return referenced_variable(binary->getLHS());
        }

        const clang::Expr* expression = expression_from(parent);
        if (expression == nullptr || !is_transparent_wrapper(expression)) {
            return nullptr;
        }
        current = parent;
    }
    return nullptr;
}

const clang::SwitchStmt* Analyzer::enclosing_direct_switch(
    const clang::CallExpr* call) const {
    clang::DynTypedNode current = clang::DynTypedNode::create(*call);
    for (unsigned depth = 0; depth < 24U; ++depth) {
        const auto parents = context_.getParents(current);
        if (parents.empty()) {
            return nullptr;
        }
        const clang::DynTypedNode& parent = parents[0];

        if (const auto* statement = parent.get<clang::SwitchStmt>()) {
            return statement->getCond() == expression_from(current)
                       ? statement
                       : nullptr;
        }

        const clang::Expr* expression = expression_from(parent);
        if (expression == nullptr || !is_transparent_wrapper(expression)) {
            return nullptr;
        }
        current = parent;
    }
    return nullptr;
}

const clang::IfStmt* Analyzer::enclosing_direct_if(
    const clang::CallExpr* call) const {
    clang::DynTypedNode current = clang::DynTypedNode::create(*call);
    for (unsigned depth = 0; depth < 24U; ++depth) {
        const auto parents = context_.getParents(current);
        if (parents.empty()) {
            return nullptr;
        }
        const clang::DynTypedNode& parent = parents[0];

        if (const auto* statement = parent.get<clang::IfStmt>()) {
            return statement->getCond() == expression_from(current)
                       ? statement
                       : nullptr;
        }

        const clang::Expr* expression = expression_from(parent);
        if (expression == nullptr || !carries_if_condition(expression)) {
            return nullptr;
        }
        current = parent;
    }
    return nullptr;
}

bool Analyzer::call_is_forwarded(const clang::CallExpr* call) const {
    clang::DynTypedNode current = clang::DynTypedNode::create(*call);
    for (unsigned depth = 0; depth < 24U; ++depth) {
        const auto parents = context_.getParents(current);
        if (parents.empty()) {
            return false;
        }
        const clang::DynTypedNode& parent = parents[0];

        if (const auto* statement = parent.get<clang::ReturnStmt>()) {
            return statement->getRetValue() == expression_from(current);
        }

        const clang::Expr* expression = expression_from(parent);
        if (expression == nullptr || !is_transparent_wrapper(expression)) {
            return false;
        }
        current = parent;
    }
    return false;
}

bool Analyzer::call_is_discarded_expression(const clang::CallExpr* call) const {
    clang::DynTypedNode current = clang::DynTypedNode::create(*call);
    for (unsigned depth = 0; depth < 24U; ++depth) {
        const auto parents = context_.getParents(current);
        if (parents.empty()) {
            return true;
        }
        const clang::DynTypedNode& parent = parents[0];

        if (parent.get<clang::CompoundStmt>() != nullptr) {
            return true;
        }

        if (const auto* binary = parent.get<clang::BinaryOperator>()) {
            if (binary->getOpcode() == clang::BO_Comma) {
                if (binary->getLHS() == expression_from(current)) {
                    return true;
                }
                if (binary->getRHS() == expression_from(current)) {
                    current = parent;
                    continue;
                }
            }
            return false;
        }

        const clang::Expr* expression = expression_from(parent);
        if (expression == nullptr || !is_transparent_wrapper(expression)) {
            return false;
        }
        current = parent;
    }
    return false;
}

bool Analyzer::call_is_operator(const clang::CallExpr* call) const {
    return llvm::isa<clang::CXXOperatorCallExpr>(call);
}

} // namespace returnguard::internal
