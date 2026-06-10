#include "ConditionEvaluator.hpp"

#include "Analyzer.hpp"
#include "AstUtils.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <llvm/Support/Casting.h>

namespace returnguard::internal {
namespace {

struct EvaluationTarget {
    const clang::VarDecl* variable = nullptr;
    const clang::Expr* expression = nullptr;
    const ExpressionSet* expressions = nullptr;
};

bool is_target_expression(const clang::Expr* expression, const EvaluationTarget& target) {
    expression = strip_expr(expression);
    if (expression == nullptr) {
        return false;
    }

    if (target.expression != nullptr && expression == strip_expr(target.expression)) {
        return true;
    }

    if (target.expressions != nullptr && target.expressions->contains(expression)) {
        return true;
    }

    if (target.variable != nullptr) {
        const auto* binary = llvm::dyn_cast<clang::BinaryOperator>(expression);
        if (binary != nullptr && binary->getOpcode() == clang::BO_Assign &&
            referenced_variable(binary->getLHS()) == target.variable) {
            return true;
        }
    }

    const auto* reference = llvm::dyn_cast<clang::DeclRefExpr>(expression);
    return reference != nullptr && target.variable != nullptr &&
           reference->getDecl() == target.variable;
}

SymbolicInteger symbolic_integer_impl(const clang::Expr* expression, const EvaluationTarget& target,
                                      Analyzer& analyzer) {
    expression = strip_expr(expression);
    if (is_target_expression(expression, target)) {
        return {.is_target = true, .constant = std::nullopt};
    }

    std::unordered_set<const clang::FunctionDecl*> active_funcs;
    std::unordered_set<const clang::VarDecl*> active_vars;
    auto domain = analyzer.expression_domain(expression, active_funcs, active_vars);
    if (domain && domain->finite && domain->values.size() == 1) {
        return {.is_target = false, .constant = domain->values[0].value};
    }

    return {
        .is_target = false,
        .constant = evaluate_integer(expression, analyzer.context()),
    };
}

Truth evaluate_impl(const clang::Expr* expression, const EvaluationTarget& target,
                    const llvm::APSInt& target_value, Analyzer& analyzer) {
    expression = strip_expr(expression);
    if (expression == nullptr) {
        return Truth::Unknown;
    }

    if (is_target_expression(expression, target)) {
        return target_value == 0 ? Truth::False : Truth::True;
    }

    if (const auto* unary = llvm::dyn_cast<clang::UnaryOperator>(expression)) {
        if (unary->getOpcode() == clang::UO_LNot) {
            return invert(evaluate_impl(unary->getSubExpr(), target, target_value, analyzer));
        }
    }

    if (const auto* binary = llvm::dyn_cast<clang::BinaryOperator>(expression)) {
        if (binary->getOpcode() == clang::BO_LAnd) {
            return logical_and(evaluate_impl(binary->getLHS(), target, target_value, analyzer),
                               evaluate_impl(binary->getRHS(), target, target_value, analyzer));
        }

        if (binary->getOpcode() == clang::BO_LOr) {
            return logical_or(evaluate_impl(binary->getLHS(), target, target_value, analyzer),
                              evaluate_impl(binary->getRHS(), target, target_value, analyzer));
        }

        if (binary->isComparisonOp()) {
            const SymbolicInteger lhs = symbolic_integer_impl(binary->getLHS(), target, analyzer);
            const SymbolicInteger rhs = symbolic_integer_impl(binary->getRHS(), target, analyzer);

            if (lhs.is_target && rhs.constant.has_value()) {
                return compare_values(binary->getOpcode(), target_value, *rhs.constant);
            }

            if (rhs.is_target && lhs.constant.has_value()) {
                return compare_values(binary->getOpcode(), *lhs.constant, target_value);
            }
        }
    }

    bool folded = false;
    if (expression->EvaluateAsBooleanCondition(folded, analyzer.context())) {
        return folded ? Truth::True : Truth::False;
    }

    return Truth::Unknown;
}

bool is_guard_comparison(clang::BinaryOperatorKind opcode) {
    return opcode == clang::BO_NE || opcode == clang::BO_LT || opcode == clang::BO_LE ||
           opcode == clang::BO_GT || opcode == clang::BO_GE;
}

bool is_target_comparison(const clang::Expr* expression, const EvaluationTarget& target,
                          Analyzer& analyzer, clang::BinaryOperatorKind opcode) {
    expression = strip_expr(expression);
    const auto* binary = llvm::dyn_cast_or_null<clang::BinaryOperator>(expression);
    if (binary == nullptr || binary->getOpcode() != opcode) {
        return false;
    }

    const SymbolicInteger lhs = symbolic_integer_impl(binary->getLHS(), target, analyzer);
    const SymbolicInteger rhs = symbolic_integer_impl(binary->getRHS(), target, analyzer);
    return (lhs.is_target && rhs.constant.has_value()) ||
           (rhs.is_target && lhs.constant.has_value());
}

bool is_target_guard_comparison(const clang::Expr* expression, const EvaluationTarget& target,
                                Analyzer& analyzer) {
    expression = strip_expr(expression);
    const auto* binary = llvm::dyn_cast_or_null<clang::BinaryOperator>(expression);
    if (binary == nullptr || !is_guard_comparison(binary->getOpcode())) {
        return false;
    }

    const SymbolicInteger lhs = symbolic_integer_impl(binary->getLHS(), target, analyzer);
    const SymbolicInteger rhs = symbolic_integer_impl(binary->getRHS(), target, analyzer);
    return (lhs.is_target && rhs.constant.has_value()) ||
           (rhs.is_target && lhs.constant.has_value());
}

bool is_allowed_condition_impl(const clang::Expr* expression, const EvaluationTarget& target,
                               Analyzer& analyzer) {
    expression = strip_expr(expression);
    if (expression == nullptr) {
        return false;
    }

    if (is_target_comparison(expression, target, analyzer, clang::BO_EQ)) {
        return true;
    }

    const auto* binary = llvm::dyn_cast<clang::BinaryOperator>(expression);
    if (binary != nullptr && binary->getOpcode() == clang::BO_LOr) {
        return is_allowed_condition_impl(binary->getLHS(), target, analyzer) &&
               is_allowed_condition_impl(binary->getRHS(), target, analyzer);
    }

    return false;
}

bool is_guard_condition_impl(const clang::Expr* expression, const EvaluationTarget& target,
                             Analyzer& analyzer) {
    expression = strip_expr(expression);
    if (expression == nullptr) {
        return false;
    }

    if (is_target_guard_comparison(expression, target, analyzer)) {
        return true;
    }

    if (const auto* unary = llvm::dyn_cast<clang::UnaryOperator>(expression);
        unary != nullptr && unary->getOpcode() == clang::UO_LNot) {
        return is_allowed_condition_impl(unary->getSubExpr(), target, analyzer);
    }

    const auto* binary = llvm::dyn_cast<clang::BinaryOperator>(expression);
    if (binary != nullptr &&
        (binary->getOpcode() == clang::BO_LAnd || binary->getOpcode() == clang::BO_LOr)) {
        return is_guard_condition_impl(binary->getLHS(), target, analyzer) &&
               is_guard_condition_impl(binary->getRHS(), target, analyzer);
    }

    return false;
}

} // namespace

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

Truth compare_values(clang::BinaryOperatorKind opcode, const llvm::APSInt& lhs,
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

SymbolicInteger symbolic_integer(const clang::Expr* expression, const clang::VarDecl* target,
                                 Analyzer& analyzer) {
    return symbolic_integer_impl(
        expression, {.variable = target, .expression = nullptr, .expressions = nullptr}, analyzer);
}

SymbolicInteger symbolic_integer(const clang::Expr* expression, const clang::Expr* target,
                                 Analyzer& analyzer) {
    return symbolic_integer_impl(
        expression, {.variable = nullptr, .expression = target, .expressions = nullptr}, analyzer);
}

Truth evaluate_condition_for_value(const clang::Expr* expression, const clang::VarDecl* target,
                                   const llvm::APSInt& target_value, Analyzer& analyzer) {
    return evaluate_impl(expression,
                         {.variable = target, .expression = nullptr, .expressions = nullptr},
                         target_value, analyzer);
}

Truth evaluate_condition_for_value(const clang::Expr* expression, const clang::Expr* target,
                                   const llvm::APSInt& target_value, Analyzer& analyzer) {
    return evaluate_impl(expression,
                         {.variable = nullptr, .expression = target, .expressions = nullptr},
                         target_value, analyzer);
}

Truth evaluate_condition_for_value(const clang::Expr* expression, const ExpressionSet& targets,
                                   const llvm::APSInt& target_value, Analyzer& analyzer) {
    return evaluate_impl(expression,
                         {.variable = nullptr, .expression = nullptr, .expressions = &targets},
                         target_value, analyzer);
}

bool is_guard_condition(const clang::Expr* expression, const clang::VarDecl* target,
                        Analyzer& analyzer) {
    return is_guard_condition_impl(
        expression, {.variable = target, .expression = nullptr, .expressions = nullptr}, analyzer);
}

bool is_guard_condition(const clang::Expr* expression, const clang::Expr* target,
                        Analyzer& analyzer) {
    return is_guard_condition_impl(
        expression, {.variable = nullptr, .expression = target, .expressions = nullptr}, analyzer);
}

bool is_guard_condition(const clang::Expr* expression, const ExpressionSet& targets,
                        Analyzer& analyzer) {
    return is_guard_condition_impl(
        expression, {.variable = nullptr, .expression = nullptr, .expressions = &targets},
        analyzer);
}

} // namespace returnguard::internal
