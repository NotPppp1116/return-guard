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

HandlerFinder::HandlerFinder(Analyzer& analyzer, const clang::VarDecl* variable,
                             clang::SourceLocation after, const Domain& domain)
    : analyzer_(analyzer), variable_(variable), after_(after), domain_(domain),
      covered_(domain.values.size(), false) {}

bool HandlerFinder::has_any_use() const { return has_any_use_; }

bool HandlerFinder::has_any_check() const { return has_any_check_; }

bool HandlerFinder::exhaustive() const { return exhaustive_; }

const std::vector<bool>& HandlerFinder::covered() const { return covered_; }

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
    const CheckResult result = analyzer_.analyze_if_chain(statement, variable_, domain_);

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

    exhaustive_ = std::all_of(covered_.begin(), covered_.end(), [](bool value) { return value; });
}

void HandlerFinder::mark_condition(const clang::Expr* condition) {
    has_any_check_ = true;
    const CheckResult result = analyzer_.analyze_condition(condition, variable_, domain_);

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

    exhaustive_ = std::all_of(covered_.begin(), covered_.end(), [](bool value) { return value; });
}

bool HandlerFinder::VisitSwitchStmt(clang::SwitchStmt* statement) {
    if (invalidated_ || !occurs_after(statement->getSwitchLoc())) {
        return true;
    }
    if (!expression_references_variable(statement->getCond(), variable_)) {
        return true;
    }
    mark_switch(statement);
    return true;
}

bool HandlerFinder::VisitIfStmt(clang::IfStmt* statement) {
    if (invalidated_ || !occurs_after(statement->getIfLoc())) {
        return true;
    }
    if (!expression_references_variable(statement->getCond(), variable_)) {
        return true;
    }
    mark_if_chain(statement);
    return true;
}

bool HandlerFinder::VisitWhileStmt(clang::WhileStmt* statement) {
    if (invalidated_ || !occurs_after(statement->getWhileLoc()) ||
        !expression_references_variable(statement->getCond(), variable_)) {
        return true;
    }
    mark_condition(statement->getCond());
    return true;
}

bool HandlerFinder::VisitDoStmt(clang::DoStmt* statement) {
    if (invalidated_ || !occurs_after(statement->getDoLoc()) ||
        !expression_references_variable(statement->getCond(), variable_)) {
        return true;
    }
    mark_condition(statement->getCond());
    return true;
}

bool HandlerFinder::VisitForStmt(clang::ForStmt* statement) {
    if (invalidated_ || !occurs_after(statement->getForLoc()) || statement->getCond() == nullptr ||
        !expression_references_variable(statement->getCond(), variable_)) {
        return true;
    }
    mark_condition(statement->getCond());
    return true;
}

bool HandlerFinder::VisitBinaryOperator(clang::BinaryOperator* expression) {
    if (invalidated_ || !expression->isAssignmentOp() ||
        !occurs_after(expression->getOperatorLoc()) ||
        referenced_variable(expression->getLHS()) != variable_) {
        return true;
    }

    if (expression->getOpcode() != clang::BO_Assign ||
        expression_references_variable(expression->getRHS(), variable_)) {
        has_any_use_ = true;
    }
    invalidated_ = true;
    return true;
}

bool HandlerFinder::VisitUnaryOperator(clang::UnaryOperator* expression) {
    if (invalidated_ || !occurs_after(expression->getOperatorLoc()) ||
        referenced_variable(expression->getSubExpr()) != variable_) {
        return true;
    }

    switch (expression->getOpcode()) {
    case clang::UO_PreInc:
    case clang::UO_PreDec:
    case clang::UO_PostInc:
    case clang::UO_PostDec:
        has_any_use_ = true;
        invalidated_ = true;
        break;
    default:
        break;
    }
    return true;
}

bool HandlerFinder::VisitDeclRefExpr(clang::DeclRefExpr* reference) {
    if (!invalidated_ && reference->getDecl() == variable_ &&
        occurs_after(reference->getExprLoc())) {
        has_any_use_ = true;
    }
    return true;
}

} // namespace returnguard::internal
