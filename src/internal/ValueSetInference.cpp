#include "ValueSetInference.hpp"

#include "Analyzer.hpp"
#include "AstUtils.hpp"
#include "DomainUtils.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/Stmt.h>
#include <llvm/ADT/APSInt.h>
#include <llvm/Support/Casting.h>

#include <algorithm>
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
            for (const auto& val : false_domain->values) {
                for (const auto& label : val.labels) {
                    add_domain_value(combined, val.value, label);
                }
                if (val.labels.empty()) {
                    add_domain_value(combined, val.value, "");
                }
            }
            combined.inferred_from_body = true;
            return combined;
        }
    }

    if (const auto* call = llvm::dyn_cast<clang::CallExpr>(expression)) {
        if (const auto* callee = call->getDirectCallee()) {
            Domain d = analyzer_.function_domain(callee, active_functions);
            if (d.finite) {
                return d;
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

    for (const auto& l : lhs->values) {
        for (const auto& r : rhs->values) {
            llvm::APSInt val = l.value;
            switch (binary->getOpcode()) {
            case clang::BO_Add:
                val += r.value;
                break;
            case clang::BO_Sub:
                val -= r.value;
                break;
            case clang::BO_Mul:
                val *= r.value;
                break;
            case clang::BO_Div:
                if (r.value == 0)
                    return std::nullopt;
                val /= r.value;
                break;
            case clang::BO_Rem:
                if (r.value == 0)
                    return std::nullopt;
                val %= r.value;
                break;
            case clang::BO_And:
                val &= r.value;
                break;
            case clang::BO_Or:
                val |= r.value;
                break;
            case clang::BO_Xor:
                val ^= r.value;
                break;
            case clang::BO_Shl:
            case clang::BO_Shr: {
                int64_t shift = r.value.getExtValue();
                if (shift < 0 || static_cast<uint64_t>(shift) >= val.getBitWidth()) {
                    return std::nullopt;
                }
                if (binary->getOpcode() == clang::BO_Shl) {
                    val <<= static_cast<unsigned>(shift);
                } else {
                    val >>= static_cast<unsigned>(shift);
                }
                break;
            }
            default:
                return std::nullopt;
            }
            add_domain_value(result, val, "");
        }
    }

    if (result.values.size() > 128)
        return std::nullopt; // Safety limit
    return result;
}

std::optional<Domain>
ValueSetInference::infer_unary(const clang::UnaryOperator* unary,
                               std::unordered_set<const clang::FunctionDecl*>& active_functions,
                               std::unordered_set<const clang::VarDecl*>& active_variables) {
    auto sub = infer_expression(unary->getSubExpr(), active_functions, active_variables);
    if (!sub || !sub->finite) {
        return std::nullopt;
    }

    Domain result;
    result.finite = true;
    result.inferred_from_body = true;
    result.type_name = unary->getType().getAsString();

    for (const auto& s : sub->values) {
        llvm::APSInt val = s.value;
        switch (unary->getOpcode()) {
        case clang::UO_Minus:
            val = -val;
            break;
        case clang::UO_Plus:
            break;
        case clang::UO_Not:
            val = ~val;
            break;
        case clang::UO_LNot:
            val = llvm::APSInt(llvm::APInt(1U, val == 0 ? 1U : 0U), false);
            break;
        default:
            return std::nullopt;
        }
        add_domain_value(result, val, "");
    }

    return result;
}

namespace {
class SimpleAssignmentFinder : public clang::RecursiveASTVisitor<SimpleAssignmentFinder> {
  public:
    SimpleAssignmentFinder(const clang::VarDecl* target) : target_(target) {}
    bool VisitBinaryOperator(clang::BinaryOperator* op) {
        if (op->isAssignmentOp() && referenced_variable(op->getLHS()) == target_) {
            if (op->getOpcode() == clang::BO_Assign) {
                assignments_.push_back(op->getRHS());
            } else {
                valid_ = false;
            }
        }
        return true;
    }
    bool VisitUnaryOperator(clang::UnaryOperator* op) {
        if (op->isIncrementDecrementOp() && referenced_variable(op->getSubExpr()) == target_) {
            valid_ = false;
        }
        return true;
    }
    bool valid_ = true;
    std::vector<const clang::Expr*> assignments_;
    const clang::VarDecl* target_;
};
} // namespace

std::optional<Domain>
ValueSetInference::infer_variable(const clang::VarDecl* variable, const clang::Expr* reference_site,
                                  std::unordered_set<const clang::FunctionDecl*>& active_functions,
                                  std::unordered_set<const clang::VarDecl*>& active_variables) {
    if (llvm::isa<clang::ParmVarDecl>(variable)) {
        return std::nullopt;
    }

    if (!variable->isLocalVarDecl()) {
        return std::nullopt;
    }

    if (active_variables.contains(variable)) {
        return std::nullopt;
    }
    active_variables.insert(variable);

    const clang::DeclContext* dc = variable->getDeclContext();
    const auto* function = llvm::dyn_cast_or_null<clang::FunctionDecl>(dc);
    if (!function || !function->hasBody()) {
        active_variables.erase(variable);
        return std::nullopt;
    }

    SimpleAssignmentFinder finder(variable);
    if (variable->hasInit()) {
        finder.assignments_.push_back(variable->getInit());
    }
    finder.TraverseStmt(const_cast<clang::Stmt*>(function->getBody()));

    if (!finder.valid_ || finder.assignments_.empty()) {
        active_variables.erase(variable);
        return std::nullopt;
    }

    Domain combined;
    combined.finite = true;
    combined.inferred_from_body = true;
    combined.type_name = variable->getType().getAsString();

    for (const auto* assignment : finder.assignments_) {
        auto part = infer_expression(assignment, active_functions, active_variables);
        if (!part || !part->finite) {
            combined.finite = false;
            break;
        }
        for (const auto& val : part->values) {
            for (const auto& label : val.labels) {
                add_domain_value(combined, val.value, label);
            }
            if (val.labels.empty()) {
                add_domain_value(combined, val.value, "");
            }
        }
    }

    active_variables.erase(variable);
    if (combined.finite)
        return combined;
    return std::nullopt;
}

} // namespace returnguard::internal
