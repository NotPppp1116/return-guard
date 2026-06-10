#include "ValueSetInference.hpp"

#include "Analyzer.hpp"
#include "AstUtils.hpp"
#include "DomainUtils.hpp"
#include "ReachingDefinitions.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <llvm/ADT/APSInt.h>
#include <llvm/Support/Casting.h>

#include <algorithm>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace returnguard::internal {

ValueSetInference::ValueSetInference(Analyzer& analyzer)
    : analyzer_(analyzer), context_(analyzer.context()) {}

std::optional<Domain> ValueSetInference::infer_expression(
    const clang::Expr* expression, std::unordered_set<const clang::FunctionDecl*>& active_functions,
    std::unordered_set<const clang::VarDecl*>& active_variables) {
    expression = strip_expr(expression);
    if (expression == nullptr) {
        return std::nullopt;
    }

    if (const std::optional<llvm::APSInt> value = evaluate_integer(expression, context_)) {
        Domain domain;
        domain.finite = true;
        domain.inferred_from_body = true;
        domain.type_name = expression->getType().getAsString();
        add_domain_value(domain, *value, "");
        return domain;
    }

    if (const auto* reference = llvm::dyn_cast<clang::DeclRefExpr>(expression)) {
        if (const auto* variable = llvm::dyn_cast<clang::VarDecl>(reference->getDecl())) {
            return infer_variable(variable, reference, active_functions, active_variables);
        }
    }

    if (const auto* binary = llvm::dyn_cast<clang::BinaryOperator>(expression)) {
        return infer_binary(binary, active_functions, active_variables);
    }

    if (const auto* unary = llvm::dyn_cast<clang::UnaryOperator>(expression)) {
        return infer_unary(unary, active_functions, active_variables);
    }

    if (const auto* conditional = llvm::dyn_cast<clang::ConditionalOperator>(expression)) {
        auto true_domain =
            infer_expression(conditional->getTrueExpr(), active_functions, active_variables);
        auto false_domain =
            infer_expression(conditional->getFalseExpr(), active_functions, active_variables);
        if (true_domain && true_domain->finite && false_domain && false_domain->finite) {
            Domain combined = *true_domain;
            for (const auto& value : false_domain->values) {
                for (const auto& label : value.labels) {
                    add_domain_value(combined, value.value, label);
                }
                if (value.labels.empty()) {
                    add_domain_value(combined, value.value, "");
                }
            }
            combined.inferred_from_body = true;
            return combined;
        }
    }

    if (const auto* call = llvm::dyn_cast<clang::CallExpr>(expression)) {
        if (const auto* callee = call->getDirectCallee()) {
            Domain domain = analyzer_.function_domain(callee, active_functions);
            if (domain.finite) {
                return domain;
            }
        }
    }

    return std::nullopt;
}

std::optional<Domain>
ValueSetInference::infer_binary(const clang::BinaryOperator* binary,
                                std::unordered_set<const clang::FunctionDecl*>& active_functions,
                                std::unordered_set<const clang::VarDecl*>& active_variables) {
    if (binary->getOpcode() == clang::BO_Comma) {
        return infer_expression(binary->getRHS(), active_functions, active_variables);
    }

    if (!binary->isAdditiveOp() && !binary->isMultiplicativeOp() && !binary->isBitwiseOp() &&
        !binary->isShiftOp()) {
        return std::nullopt;
    }

    auto lhs = infer_expression(binary->getLHS(), active_functions, active_variables);
    auto rhs = infer_expression(binary->getRHS(), active_functions, active_variables);

    if (!lhs || !lhs->finite || !rhs || !rhs->finite) {
        return std::nullopt;
    }

    Domain result;
    result.finite = true;
    result.inferred_from_body = true;
    result.type_name = binary->getType().getAsString();

    for (const auto& left : lhs->values) {
        for (const auto& right : rhs->values) {
            llvm::APSInt value = left.value;
            switch (binary->getOpcode()) {
            case clang::BO_Add:
                value += right.value;
                break;
            case clang::BO_Sub:
                value -= right.value;
                break;
            case clang::BO_Mul:
                value *= right.value;
                break;
            case clang::BO_Div:
                if (right.value == 0) {
                    return std::nullopt;
                }
                value /= right.value;
                break;
            case clang::BO_Rem:
                if (right.value == 0) {
                    return std::nullopt;
                }
                value %= right.value;
                break;
            case clang::BO_And:
                value &= right.value;
                break;
            case clang::BO_Or:
                value |= right.value;
                break;
            case clang::BO_Xor:
                value ^= right.value;
                break;
            case clang::BO_Shl:
            case clang::BO_Shr: {
                const int64_t shift = right.value.getExtValue();
                if (shift < 0 || static_cast<uint64_t>(shift) >= value.getBitWidth()) {
                    return std::nullopt;
                }
                if (binary->getOpcode() == clang::BO_Shl) {
                    value <<= static_cast<unsigned>(shift);
                } else {
                    value >>= static_cast<unsigned>(shift);
                }
                break;
            }
            default:
                return std::nullopt;
            }
            add_domain_value(result, value, "");
            if (result.values.size() > 128U) {
                return std::nullopt;
            }
        }
    }

    return result;
}

std::optional<Domain>
ValueSetInference::infer_unary(const clang::UnaryOperator* unary,
                               std::unordered_set<const clang::FunctionDecl*>& active_functions,
                               std::unordered_set<const clang::VarDecl*>& active_variables) {
    auto operand = infer_expression(unary->getSubExpr(), active_functions, active_variables);
    if (!operand || !operand->finite) {
        return std::nullopt;
    }

    Domain result;
    result.finite = true;
    result.inferred_from_body = true;
    result.type_name = unary->getType().getAsString();

    for (const auto& source : operand->values) {
        llvm::APSInt value = source.value;
        switch (unary->getOpcode()) {
        case clang::UO_Minus:
            value = -value;
            break;
        case clang::UO_Plus:
            break;
        case clang::UO_Not:
            value = ~value;
            break;
        case clang::UO_LNot:
            value = llvm::APSInt(llvm::APInt(1U, value == 0 ? 1U : 0U), false);
            break;
        default:
            return std::nullopt;
        }
        add_domain_value(result, value, "");
    }

    return result;
}

std::optional<Domain>
ValueSetInference::infer_variable(const clang::VarDecl* variable, const clang::Expr* reference_site,
                                  std::unordered_set<const clang::FunctionDecl*>& active_functions,
                                  std::unordered_set<const clang::VarDecl*>& active_variables) {
    if (variable == nullptr || reference_site == nullptr ||
        llvm::isa<clang::ParmVarDecl>(variable) || !variable->isLocalVarDecl() ||
        variable->getType().isVolatileQualified()) {
        return std::nullopt;
    }

    if (active_variables.contains(variable)) {
        return std::nullopt;
    }

    const auto* function = llvm::dyn_cast<clang::FunctionDecl>(variable->getDeclContext());
    if (function == nullptr || !function->hasBody()) {
        return std::nullopt;
    }

    const ReachingDefinitions reaching =
        reaching_definitions_at(*function, *variable, *reference_site,
                                const_cast<clang::ASTContext&>(context_));
    if (reaching.unknown || reaching.expressions.empty()) {
        return std::nullopt;
    }

    active_variables.insert(variable);

    Domain combined;
    combined.finite = true;
    combined.inferred_from_body = true;
    combined.type_name = variable->getType().getAsString();

    for (const clang::Expr* definition : reaching.expressions) {
        std::optional<Domain> part =
            infer_expression(definition, active_functions, active_variables);
        if (!part || !part->finite) {
            combined.finite = false;
            break;
        }
        for (const auto& value : part->values) {
            for (const auto& label : value.labels) {
                add_domain_value(combined, value.value, label);
            }
            if (value.labels.empty()) {
                add_domain_value(combined, value.value, "");
            }
        }
        if (combined.values.size() > 128U) {
            combined.finite = false;
            break;
        }
    }

    active_variables.erase(variable);
    if (combined.finite && !combined.values.empty()) {
        return combined;
    }
    return std::nullopt;
}

} // namespace returnguard::internal
