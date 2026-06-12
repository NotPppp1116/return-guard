#include "Analyzer.hpp"

#include "AstUtils.hpp"
#include "ConditionEvaluator.hpp"
#include "DomainUtils.hpp"
#include "IfChain.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <clang/AST/Stmt.h>
#include <llvm/Support/Casting.h>

#include <vector>

namespace returnguard::internal {
namespace {

bool exits_on_all_failure_values(const clang::IfStmt* statement, const ExpressionSet& aliases,
                                 const Domain& domain, Analyzer& analyzer) {
    if (statement == nullptr || !domain.fallible_contract || !domain.finite ||
        !statement_exits(statement->getThen())) {
        return false;
    }

    bool saw_failure = false;
    for (const DomainValue& value : domain.values) {
        if (!is_failure_value(value)) {
            continue;
        }
        saw_failure = true;
        if (evaluate_condition_for_value(statement->getCond(), aliases, value.value, analyzer) !=
            Truth::True) {
            return false;
        }
    }
    return saw_failure;
}

} // namespace

CheckResult Analyzer::analyze_if_chain(const clang::IfStmt* statement, const ExpressionSet& aliases,
                                       const Domain& domain) const {
    CheckResult result;
    result.kind = HandlingKind::PartiallyChecked;

    if (has_final_else(statement)) {
        result.kind = HandlingKind::ExhaustivelyChecked;
        return result;
    }

    if (domain.fallible_contract
            ? exits_on_all_failure_values(statement, aliases, domain,
                                          const_cast<Analyzer&>(*this))
            : (is_guard_condition(statement->getCond(), aliases,
                                  const_cast<Analyzer&>(*this)) &&
               statement_exits(statement->getThen()))) {
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
            if (evaluate_condition_for_value(current->getCond(), aliases, value.value,
                                             const_cast<Analyzer&>(*this)) == Truth::True) {
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

CheckResult Analyzer::analyze_condition(const clang::Expr* condition, const ExpressionSet& aliases,
                                        const Domain& domain) const {
    CheckResult result;
    result.kind = HandlingKind::PartiallyChecked;

    if (!domain.finite) {
        result.detail = "conditional checks through an alias have no exhaustive fallback";
        return result;
    }

    for (const DomainValue& value : domain.values) {
        if (evaluate_condition_for_value(condition, aliases, value.value,
                                         const_cast<Analyzer&>(*this)) != Truth::True) {
            result.missing.push_back(value);
        }
    }

    if (result.missing.empty()) {
        result.kind = HandlingKind::ExhaustivelyChecked;
    }
    return result;
}

} // namespace returnguard::internal
