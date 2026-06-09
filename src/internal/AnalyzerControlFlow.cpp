#include "Analyzer.hpp"

#include "AstUtils.hpp"
#include "ConditionEvaluator.hpp"
#include "DirectIf.hpp"
#include "DomainUtils.hpp"
#include "IfChain.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Stmt.h>
#include <llvm/Support/Casting.h>

#include <optional>
#include <vector>

namespace returnguard::internal {

CheckResult Analyzer::analyze_switch(const clang::SwitchStmt* statement,
                                     const Domain& domain) const {
    CheckResult result;
    result.kind = HandlingKind::PartiallyChecked;

    std::vector<bool> covered(domain.values.size(), false);
    bool has_default = false;

    for (const clang::SwitchCase* current = statement->getSwitchCaseList(); current != nullptr;
         current = current->getNextSwitchCase()) {
        if (llvm::isa<clang::DefaultStmt>(current)) {
            has_default = true;
            continue;
        }

        const auto* case_statement = llvm::cast<clang::CaseStmt>(current);
        const std::optional<llvm::APSInt> lhs =
            evaluate_integer(case_statement->getLHS(), context_);
        if (!lhs.has_value()) {
            continue;
        }

        const std::optional<llvm::APSInt> rhs =
            case_statement->getRHS() == nullptr
                ? std::optional<llvm::APSInt>{}
                : evaluate_integer(case_statement->getRHS(), context_);

        for (std::size_t index = 0; index < domain.values.size(); ++index) {
            const llvm::APSInt& value = domain.values[index].value;
            if (!rhs.has_value()) {
                if (same_value(value, *lhs)) {
                    covered[index] = true;
                }
                continue;
            }

            if (llvm::APSInt::compareValues(value, *lhs) >= 0 &&
                llvm::APSInt::compareValues(value, *rhs) <= 0) {
                covered[index] = true;
            }
        }
    }

    if (has_default) {
        result.kind = HandlingKind::ExhaustivelyChecked;
        return result;
    }

    if (!domain.finite) {
        result.detail = "switch on an open-ended return domain has no default";
        return result;
    }

    for (std::size_t index = 0; index < domain.values.size(); ++index) {
        if (!covered[index]) {
            result.missing.push_back(domain.values[index]);
        }
    }

    if (result.missing.empty()) {
        result.kind = HandlingKind::ExhaustivelyChecked;
    }
    return result;
}

CheckResult Analyzer::analyze_if_chain(const clang::IfStmt* statement,
                                       const clang::VarDecl* variable, const Domain& domain) const {
    CheckResult result;
    result.kind = HandlingKind::PartiallyChecked;

    if (has_final_else(statement)) {
        result.kind = HandlingKind::ExhaustivelyChecked;
        return result;
    }

    if (variable != nullptr && is_guard_condition(statement->getCond(), variable, context_) &&
        statement_exits(statement->getThen())) {
        result.kind = HandlingKind::ExhaustivelyChecked;
        return result;
    }

    if (!domain.finite || variable == nullptr) {
        result.detail = "conditional checks have no final else";
        return result;
    }

    const std::vector<const clang::IfStmt*> chain = if_chain(statement);
    for (const DomainValue& value : domain.values) {
        bool handled = false;
        for (const clang::IfStmt* current : chain) {
            if (evaluate_condition_for_value(current->getCond(), variable, value.value, context_) ==
                Truth::True) {
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

CheckResult Analyzer::analyze_condition(const clang::Expr* condition,
                                        const clang::VarDecl* variable,
                                        const Domain& domain) const {
    CheckResult result;
    result.kind = HandlingKind::PartiallyChecked;

    if (!domain.finite || variable == nullptr) {
        result.detail = "conditional check has no exhaustive fallback";
        return result;
    }

    for (const DomainValue& value : domain.values) {
        if (evaluate_condition_for_value(condition, variable, value.value, context_) !=
            Truth::True) {
            result.missing.push_back(value);
        }
    }

    if (result.missing.empty()) {
        result.kind = HandlingKind::ExhaustivelyChecked;
    }
    return result;
}

CheckResult analyze_direct_condition(const clang::Expr* condition, const clang::Expr* target,
                                     const Domain& domain, const clang::ASTContext& context) {
    CheckResult result;
    result.kind = HandlingKind::PartiallyChecked;

    if (!domain.finite || target == nullptr) {
        result.detail = "direct conditional check has no exhaustive fallback";
        return result;
    }

    for (const DomainValue& value : domain.values) {
        if (evaluate_condition_for_value(condition, target, value.value, context) != Truth::True) {
            result.missing.push_back(value);
        }
    }

    if (result.missing.empty()) {
        result.kind = HandlingKind::ExhaustivelyChecked;
    }
    return result;
}

CheckResult analyze_direct_fallback_condition(const clang::Expr* condition,
                                              const clang::Expr* target) {
    CheckResult result;
    if (condition == nullptr || target == nullptr) {
        result.kind = HandlingKind::PartiallyChecked;
        result.detail = "direct conditional check has no fallback";
        return result;
    }

    result.kind = HandlingKind::ExhaustivelyChecked;
    return result;
}

CheckResult analyze_direct_if(const clang::IfStmt* statement, const clang::Expr* target,
                              const Domain& domain, const clang::ASTContext& context) {
    if (statement == nullptr) {
        CheckResult result;
        result.kind = HandlingKind::PartiallyChecked;
        result.detail = "direct conditional check has no exhaustive fallback";
        return result;
    }

    if (has_final_else(statement)) {
        CheckResult result;
        result.kind = HandlingKind::ExhaustivelyChecked;
        return result;
    }

    if (is_guard_condition(statement->getCond(), target, context) &&
        statement_exits(statement->getThen())) {
        CheckResult result;
        result.kind = HandlingKind::ExhaustivelyChecked;
        return result;
    }

    CheckResult result = analyze_direct_condition(statement->getCond(), target, domain, context);
    if (!result.detail.empty()) {
        result.detail = "direct conditional checks have no final else";
    }
    return result;
}

} // namespace returnguard::internal
