#include "ConditionEvaluator.hpp"

#include "AstUtils.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <llvm/Support/Casting.h>

namespace returnguard::internal {

Truth invert(Truth value) {
    if (value == Truth::True) {
        return Truth::False;
    }
    if (value == Truth::False) {
        return Truth::True;
    }
    return Truth::Unknown;
}

Truth logical_and(Truth lhs, Truth rhs) {
    if (lhs == Truth::False || rhs == Truth::False) {
        return Truth::False;
    }
    if (lhs == Truth::True && rhs == Truth::True) {
        return Truth::True;
    }
    return Truth::Unknown;
}

Truth logical_or(Truth lhs, Truth rhs) {
    if (lhs == Truth::True || rhs == Truth::True) {
        return Truth::True;
    }
    if (lhs == Truth::False && rhs == Truth::False) {
        return Truth::False;
    }
    return Truth::Unknown;
}

Truth compare_values(
    clang::BinaryOperatorKind opcode,
    const llvm::APSInt& lhs,
    const llvm::APSInt& rhs) {
    const int comparison = llvm::APSInt::compareValues(lhs, rhs);

    switch (opcode) {
    case clang::BO_EQ:
        return comparison == 0 ? Truth::True : Truth::False;
    case clang::BO_NE:
        return comparison != 0 ? Truth::True : Truth::False;
    case clang::BO_LT:
        return comparison < 0 ? Truth::True : Truth::False;
    case clang::BO_LE:
        return comparison <= 0 ? Truth::True : Truth::False;
    case clang::BO_GT:
        return comparison > 0 ? Truth::True : Truth::False;
    case clang::BO_GE:
        return comparison >= 0 ? Truth::True : Truth::False;
    default:
        return Truth::Unknown;
    }
}

SymbolicInteger symbolic_integer(
    const clang::Expr* expression,
    const clang::VarDecl* target,
    const clang::ASTContext& context) {
    expression = strip_expr(expression);

    if (const auto* reference =
            llvm::dyn_cast_or_null<clang::DeclRefExpr>(expression)) {
        if (reference->getDecl() == target) {
            return {.is_target = true, .constant = std::nullopt};
        }
    }

    return {
        .is_target = false,
        .constant = evaluate_integer(expression, context),
    };
}

Truth evaluate_condition_for_value(
    const clang::Expr* expression,
    const clang::VarDecl* target,
    const llvm::APSInt& target_value,
    const clang::ASTContext& context) {
    expression = strip_expr(expression);
    if (expression == nullptr) {
        return Truth::Unknown;
    }

    if (const auto* reference = llvm::dyn_cast<clang::DeclRefExpr>(expression)) {
        if (reference->getDecl() == target) {
            return target_value == 0 ? Truth::False : Truth::True;
        }
    }

    if (const auto* unary = llvm::dyn_cast<clang::UnaryOperator>(expression)) {
        if (unary->getOpcode() == clang::UO_LNot) {
            return invert(evaluate_condition_for_value(
                unary->getSubExpr(), target, target_value, context));
        }
    }

    if (const auto* binary = llvm::dyn_cast<clang::BinaryOperator>(expression)) {
        if (binary->getOpcode() == clang::BO_LAnd) {
            return logical_and(
                evaluate_condition_for_value(
                    binary->getLHS(), target, target_value, context),
                evaluate_condition_for_value(
                    binary->getRHS(), target, target_value, context));
        }

        if (binary->getOpcode() == clang::BO_LOr) {
            return logical_or(
                evaluate_condition_for_value(
                    binary->getLHS(), target, target_value, context),
                evaluate_condition_for_value(
                    binary->getRHS(), target, target_value, context));
        }

        if (binary->isComparisonOp()) {
            const SymbolicInteger lhs =
                symbolic_integer(binary->getLHS(), target, context);
            const SymbolicInteger rhs =
                symbolic_integer(binary->getRHS(), target, context);

            if (lhs.is_target && rhs.constant.has_value()) {
                return compare_values(
                    binary->getOpcode(), target_value, *rhs.constant);
            }

            if (rhs.is_target && lhs.constant.has_value()) {
                return compare_values(
                    binary->getOpcode(), *lhs.constant, target_value);
            }
        }
    }

    bool folded = false;
    if (expression->EvaluateAsBooleanCondition(folded, context)) {
        return folded ? Truth::True : Truth::False;
    }

    return Truth::Unknown;
}

} // namespace returnguard::internal
