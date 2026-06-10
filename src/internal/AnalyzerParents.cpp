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
    return llvm::isa<clang::ParenExpr, clang::ImplicitCastExpr, clang::CStyleCastExpr,
                     clang::CXXStaticCastExpr, clang::CXXFunctionalCastExpr,
                     clang::ExprWithCleanups, clang::MaterializeTemporaryExpr,
                     clang::CXXBindTemporaryExpr>(expression);
}

const clang::Expr* expression_from(const clang::DynTypedNode& node) {
    return node.get<clang::Expr>();
}

bool is_comma_rhs(const clang::Expr* parent, const clang::Expr* child) {
    const auto* binary = llvm::dyn_cast_or_null<clang::BinaryOperator>(parent);
    return binary != nullptr && binary->getOpcode() == clang::BO_Comma && binary->getRHS() == child;
}

bool propagates_value(const clang::Expr* parent, const clang::Expr* child) {
    if (is_transparent_wrapper(parent) || is_comma_rhs(parent, child)) {
        return true;
    }

    if (const auto* binary = llvm::dyn_cast<clang::BinaryOperator>(parent)) {
        return binary->isAdditiveOp() || binary->isMultiplicativeOp() || binary->isBitwiseOp() ||
               binary->isShiftOp();
    }

    if (const auto* unary = llvm::dyn_cast<clang::UnaryOperator>(parent)) {
        return unary->getOpcode() == clang::UO_Minus || unary->getOpcode() == clang::UO_Plus ||
               unary->getOpcode() == clang::UO_Not || unary->getOpcode() == clang::UO_LNot;
    }

    if (llvm::isa<clang::ConditionalOperator>(parent)) {
        return true;
    }

    return false;
}

bool carries_if_condition(const clang::Expr* parent, const clang::Expr* child) {
    if (propagates_value(parent, child)) {
        return true;
    }

    if (const auto* unary = llvm::dyn_cast<clang::UnaryOperator>(parent)) {
        return unary->getOpcode() == clang::UO_LNot;
    }

    if (const auto* binary = llvm::dyn_cast<clang::BinaryOperator>(parent)) {
        return binary->isComparisonOp() || binary->getOpcode() == clang::BO_LAnd ||
               binary->getOpcode() == clang::BO_LOr;
    }

    return false;
}

bool is_simple_assignment_to(const clang::Expr* expression, const clang::Expr* child,
                             const clang::VarDecl* variable) {
    const auto* binary = llvm::dyn_cast_or_null<clang::BinaryOperator>(expression);
    return binary != nullptr && binary->getOpcode() == clang::BO_Assign &&
           binary->getRHS() == child && referenced_variable(binary->getLHS()) == variable;
}

const clang::Expr* matching_condition_parent(const clang::DynTypedNode& parent,
                                             const clang::Expr* child) {
    if (const auto* statement = parent.get<clang::IfStmt>()) {
        return statement->getCond() == child ? statement->getCond() : nullptr;
    }

    if (const auto* statement = parent.get<clang::WhileStmt>()) {
        return statement->getCond() == child ? statement->getCond() : nullptr;
    }

    if (const auto* statement = parent.get<clang::DoStmt>()) {
        return statement->getCond() == child ? statement->getCond() : nullptr;
    }

    if (const auto* statement = parent.get<clang::ForStmt>()) {
        return statement->getCond() == child ? statement->getCond() : nullptr;
    }

    return nullptr;
}

const clang::Expr* matching_conditional_operator_parent(const clang::DynTypedNode& parent,
                                                        const clang::Expr* child) {
    if (const auto* expression = parent.get<clang::ConditionalOperator>()) {
        return expression->getCond() == child ? expression->getCond() : nullptr;
    }
    return nullptr;
}

} // namespace

const clang::VarDecl* Analyzer::variable_initialized_by_call(const clang::CallExpr* call) const {
    clang::DynTypedNode current = clang::DynTypedNode::create(*call);
    for (unsigned depth = 0; depth < 24U; ++depth) {
        const auto parents = context_.getParents(current);
        if (parents.empty()) {
            return nullptr;
        }
        const clang::DynTypedNode& parent = parents[0];

        if (const auto* variable = parent.get<clang::VarDecl>()) {
            return variable->getInit() == expression_from(current) ? variable : nullptr;
        }

        const clang::Expr* child = expression_from(current);
        const clang::Expr* expression = expression_from(parent);
        if (child == nullptr || expression == nullptr || !propagates_value(expression, child)) {
            return nullptr;
        }
        current = parent;
    }
    return nullptr;
}

const clang::VarDecl* Analyzer::variable_assigned_from_call(const clang::CallExpr* call) const {
    clang::DynTypedNode current = clang::DynTypedNode::create(*call);
    for (unsigned depth = 0; depth < 24U; ++depth) {
        const auto parents = context_.getParents(current);
        if (parents.empty()) {
            return nullptr;
        }
        const clang::DynTypedNode& parent = parents[0];

        if (const auto* binary = parent.get<clang::BinaryOperator>();
            binary != nullptr && binary->isAssignmentOp()) {
            if (binary->getOpcode() != clang::BO_Assign ||
                binary->getRHS() != expression_from(current)) {
                return nullptr;
            }
            return referenced_variable(binary->getLHS());
        }

        const clang::Expr* child = expression_from(current);
        const clang::Expr* expression = expression_from(parent);
        if (child == nullptr || expression == nullptr || !propagates_value(expression, child)) {
            return nullptr;
        }
        current = parent;
    }
    return nullptr;
}

const clang::SwitchStmt* Analyzer::enclosing_direct_switch(const clang::CallExpr* call) const {
    clang::DynTypedNode current = clang::DynTypedNode::create(*call);
    for (unsigned depth = 0; depth < 24U; ++depth) {
        const auto parents = context_.getParents(current);
        if (parents.empty()) {
            return nullptr;
        }
        const clang::DynTypedNode& parent = parents[0];

        if (const auto* statement = parent.get<clang::SwitchStmt>()) {
            return statement->getCond() == expression_from(current) ? statement : nullptr;
        }

        const clang::Expr* child = expression_from(current);
        const clang::Expr* expression = expression_from(parent);
        if (child == nullptr || expression == nullptr || !propagates_value(expression, child)) {
            return nullptr;
        }
        current = parent;
    }
    return nullptr;
}

const clang::IfStmt* Analyzer::enclosing_direct_if(const clang::CallExpr* call) const {
    clang::DynTypedNode current = clang::DynTypedNode::create(*call);
    for (unsigned depth = 0; depth < 24U; ++depth) {
        const auto parents = context_.getParents(current);
        if (parents.empty()) {
            return nullptr;
        }
        const clang::DynTypedNode& parent = parents[0];

        if (const auto* statement = parent.get<clang::IfStmt>()) {
            return statement->getCond() == expression_from(current) ? statement : nullptr;
        }

        const clang::Expr* child = expression_from(current);
        const clang::Expr* expression = expression_from(parent);
        if (child == nullptr || expression == nullptr || !carries_if_condition(expression, child)) {
            return nullptr;
        }
        current = parent;
    }
    return nullptr;
}

const clang::Expr* Analyzer::enclosing_direct_loop_condition(const clang::CallExpr* call) const {
    clang::DynTypedNode current = clang::DynTypedNode::create(*call);
    for (unsigned depth = 0; depth < 24U; ++depth) {
        const auto parents = context_.getParents(current);
        if (parents.empty()) {
            return nullptr;
        }
        const clang::DynTypedNode& parent = parents[0];

        if (const auto* statement = parent.get<clang::WhileStmt>()) {
            return statement->getCond() == expression_from(current) ? statement->getCond()
                                                                    : nullptr;
        }

        if (const auto* statement = parent.get<clang::DoStmt>()) {
            return statement->getCond() == expression_from(current) ? statement->getCond()
                                                                    : nullptr;
        }

        if (const auto* statement = parent.get<clang::ForStmt>()) {
            return statement->getCond() == expression_from(current) ? statement->getCond()
                                                                    : nullptr;
        }

        const clang::Expr* child = expression_from(current);
        const clang::Expr* expression = expression_from(parent);
        if (child == nullptr || expression == nullptr || !carries_if_condition(expression, child)) {
            return nullptr;
        }
        current = parent;
    }
    return nullptr;
}

const clang::Expr*
Analyzer::enclosing_direct_conditional_condition(const clang::CallExpr* call) const {
    clang::DynTypedNode current = clang::DynTypedNode::create(*call);
    for (unsigned depth = 0; depth < 24U; ++depth) {
        const auto parents = context_.getParents(current);
        if (parents.empty()) {
            return nullptr;
        }
        const clang::DynTypedNode& parent = parents[0];

        if (const clang::Expr* condition =
                matching_conditional_operator_parent(parent, expression_from(current))) {
            return condition;
        }

        const clang::Expr* child = expression_from(current);
        const clang::Expr* expression = expression_from(parent);
        if (child == nullptr || expression == nullptr || !carries_if_condition(expression, child)) {
            return nullptr;
        }
        current = parent;
    }
    return nullptr;
}

const clang::Expr* Analyzer::enclosing_assignment_condition(const clang::CallExpr* call,
                                                            const clang::VarDecl* variable) const {
    clang::DynTypedNode current = clang::DynTypedNode::create(*call);
    bool found_assignment = false;

    for (unsigned depth = 0; depth < 24U; ++depth) {
        const auto parents = context_.getParents(current);
        if (parents.empty()) {
            return nullptr;
        }
        const clang::DynTypedNode& parent = parents[0];

        if (const clang::Expr* condition =
                matching_condition_parent(parent, expression_from(current))) {
            return found_assignment ? condition : nullptr;
        }

        const clang::Expr* child = expression_from(current);
        const clang::Expr* expression = expression_from(parent);
        if (child == nullptr || expression == nullptr) {
            return nullptr;
        }

        if (!found_assignment && is_simple_assignment_to(expression, child, variable)) {
            found_assignment = true;
            current = parent;
            continue;
        }

        if (!carries_if_condition(expression, child)) {
            return nullptr;
        }
        current = parent;
    }
    return nullptr;
}

const clang::Expr*
Analyzer::enclosing_assignment_conditional_condition(const clang::CallExpr* call,
                                                     const clang::VarDecl* variable) const {
    clang::DynTypedNode current = clang::DynTypedNode::create(*call);
    bool found_assignment = false;

    for (unsigned depth = 0; depth < 24U; ++depth) {
        const auto parents = context_.getParents(current);
        if (parents.empty()) {
            return nullptr;
        }
        const clang::DynTypedNode& parent = parents[0];

        if (const clang::Expr* condition =
                matching_conditional_operator_parent(parent, expression_from(current))) {
            return found_assignment ? condition : nullptr;
        }

        const clang::Expr* child = expression_from(current);
        const clang::Expr* expression = expression_from(parent);
        if (child == nullptr || expression == nullptr) {
            return nullptr;
        }

        if (!found_assignment && is_simple_assignment_to(expression, child, variable)) {
            found_assignment = true;
            current = parent;
            continue;
        }

        if (!carries_if_condition(expression, child)) {
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

        const clang::Expr* child = expression_from(current);
        const clang::Expr* expression = expression_from(parent);
        if (child == nullptr || expression == nullptr || !propagates_value(expression, child)) {
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

        const clang::Expr* child = expression_from(current);
        const clang::Expr* expression = expression_from(parent);
        if (child == nullptr || expression == nullptr || !is_transparent_wrapper(expression)) {
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
