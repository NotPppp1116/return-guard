#include "HandlerFinder.hpp"

#include "Analyzer.hpp"
#include "AstUtils.hpp"
#include "DomainUtils.hpp"

#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/Stmt.h>
#include <clang/Basic/SourceManager.h>

#include <algorithm>

namespace returnguard::internal {

HandlerFinder::HandlerFinder(
    Analyzer& analyzer,
    const clang::VarDecl* variable,
    clang::SourceLocation after,
    const Domain& domain)
    : analyzer_(analyzer),
      variable_(variable),
      after_(after),
      domain_(domain),
      covered_(domain.values.size(), false) {}

bool HandlerFinder::has_any_use() const {
    return has_any_use_;
}

bool HandlerFinder::has_any_check() const {
    return has_any_check_;
}

bool HandlerFinder::exhaustive() const {
    return exhaustive_;
}

const std::vector<bool>& HandlerFinder::covered() const {
    return covered_;
}

bool HandlerFinder::occurs_after(clang::SourceLocation location) const {
    if (location.isInvalid() || after_.isInvalid()) {
        return false;
    }

    const clang::SourceManager& manager = analyzer_.source_manager();
    location = manager.getFileLoc(location);
    const clang::SourceLocation after = manager.getFileLoc(after_);

    if (location.isInvalid() || after.isInvalid() ||
        manager.getFileID(location) != manager.getFileID(after)) {
        return false;
    }
    return manager.isBeforeInTranslationUnit(after, location);
}

void HandlerFinder::mark_switch(const clang::SwitchStmt* statement) {
    has_any_check_ = true;
    const CheckResult result = analyzer_.analyze_switch(statement, domain_);
    if (result.kind == HandlingKind::ExhaustivelyChecked) {
        exhaustive_ = true;
        std::fill(covered_.begin(), covered_.end(), true);
        return;
    }

    if (!domain_.finite) {
        return;
    }

    std::vector<bool> missing(domain_.values.size(), false);
    for (std::size_t index = 0; index < domain_.values.size(); ++index) {
        for (const DomainValue& value : result.missing) {
            if (same_value(domain_.values[index].value, value.value)) {
                missing[index] = true;
                break;
            }
        }
        if (!missing[index]) {
            covered_[index] = true;
        }
    }
}

void HandlerFinder::mark_if_chain(const clang::IfStmt* statement) {
    has_any_check_ = true;
    const CheckResult result =
        analyzer_.analyze_if_chain(statement, variable_, domain_);

    if (result.kind == HandlingKind::ExhaustivelyChecked) {
        exhaustive_ = true;
        std::fill(covered_.begin(), covered_.end(), true);
        return;
    }

    if (!domain_.finite) {
        return;
    }

    for (std::size_t index = 0; index < domain_.values.size(); ++index) {
        bool missing = false;
        for (const DomainValue& value : result.missing) {
            if (same_value(domain_.values[index].value, value.value)) {
                missing = true;
                break;
            }
        }
        if (!missing) {
            covered_[index] = true;
        }
    }

    exhaustive_ = std::all_of(
        covered_.begin(), covered_.end(), [](bool value) { return value; });
}

bool HandlerFinder::VisitSwitchStmt(clang::SwitchStmt* statement) {
    if (!occurs_after(statement->getSwitchLoc())) {
        return true;
    }
    if (!expression_references_variable(statement->getCond(), variable_)) {
        return true;
    }
    mark_switch(statement);
    return true;
}

bool HandlerFinder::VisitIfStmt(clang::IfStmt* statement) {
    if (!occurs_after(statement->getIfLoc())) {
        return true;
    }
    if (!expression_references_variable(statement->getCond(), variable_)) {
        return true;
    }
    mark_if_chain(statement);
    return true;
}

bool HandlerFinder::VisitDeclRefExpr(clang::DeclRefExpr* reference) {
    if (reference->getDecl() == variable_ &&
        occurs_after(reference->getExprLoc())) {
        has_any_use_ = true;
    }
    return true;
}

} // namespace returnguard::internal
