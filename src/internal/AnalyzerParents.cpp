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
            return variable;
        }

        if (parent.get<clang::Expr>() != nullptr) {
            current = parent;
            continue;
        }
        return nullptr;
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
            if (binary->isAssignmentOp()) {
                return referenced_variable(binary->getLHS());
            }
        }

        if (parent.get<clang::Expr>() != nullptr) {
            current = parent;
            continue;
        }
        return nullptr;
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
            return statement;
        }
        if (parent.get<clang::Expr>() != nullptr) {
            current = parent;
            continue;
        }
        return nullptr;
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
            return statement;
        }
        if (parent.get<clang::Expr>() != nullptr) {
            current = parent;
            continue;
        }
        return nullptr;
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
        if (parent.get<clang::ReturnStmt>() != nullptr) {
            return true;
        }
        if (parent.get<clang::Expr>() != nullptr) {
            current = parent;
            continue;
        }
        return false;
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
        if (parent.get<clang::Expr>() != nullptr) {
            current = parent;
            continue;
        }
        return false;
    }
    return false;
}

bool Analyzer::call_is_operator(const clang::CallExpr* call) const {
    return llvm::isa<clang::CXXOperatorCallExpr>(call);
}

} // namespace returnguard::internal
