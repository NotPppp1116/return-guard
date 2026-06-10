#include "Analyzer.hpp"

#include "AstUtils.hpp"
#include "ConditionEvaluator.hpp"
#include "IfChain.hpp"

#include <clang/AST/Stmt.h>

namespace returnguard::internal {

CheckResult Analyzer::analyze_if_chain(
    const clang::IfStmt* statement,
    const ExpressionSet& aliases,
    const Domain& domain) const {
    CheckResult result;
    result.kind = HandlingKind::PartiallyChecked;

    if (has_final_else(statement)) {
        result.kind = HandlingKind::ExhaustivelyChecked;
        return result;
    }

    if (is_guard_condition(statement->getCond(), aliases, context_) &&
        statement_exits(statement->getThen())) {
        result.kind = HandlingKind::ExhaustivelyChecked;
        return result;
    }

    if (!domain.finite) {
        result.detail = "conditional checks through an alias have no final else";
        return result;
    }

    const std::vector<const clang::IfStmt*> chain = if_chain(statement);
    for (const DomainValue& value : domain.values) {
        bool handled = false;
        for (const clang::IfStmt* current : chain) {
            if (evaluate_condition_for_value(
                    current->getCond(), aliases, value.value, context_) == Truth::True) {
                handled = true;
                break;
            }
        }
        if (!handled) {
            result.missing.push_back(value);
        }
    }

    if (result.missing.empty()) {
        result.kind = HandlingKind::ExhaustivelyChecked;
    }
    return result;
}

CheckResult Analyzer::analyze_condition(
    const clang::Expr* condition,
    const ExpressionSet& aliases,
    const Domain& domain) const {
    CheckResult result;
    result.kind = HandlingKind::PartiallyChecked;

    if (!domain.finite) {
        result.detail = "conditional check through an alias has no exhaustive fallback";
        return result;
    }

    for (const DomainValue& value : domain.values) {
        if (evaluate_condition_for_value(condition, aliases, value.value, context_) !=
            Truth::True) {
            result.missing.push_back(value);
        }
    }

    if (result.missing.empty()) {
        result.kind = HandlingKind::ExhaustivelyChecked;
    }
    return result;
}

} // namespace returnguard::internal
