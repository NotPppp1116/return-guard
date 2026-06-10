#include "HandlerFinder.hpp"

#include "Analyzer.hpp"
#include "AstUtils.hpp"
#include "DomainUtils.hpp"

#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/Stmt.h>
#include <clang/Basic/SourceManager.h>
#include <llvm/Support/Casting.h>

#include <algorithm>

namespace returnguard::internal {

HandlerFinder::HandlerFinder(Analyzer& analyzer, const clang::VarDecl* variable,
                             clang::SourceLocation after, const Domain& domain)
    : analyzer_(analyzer), after_(after), domain_(domain), covered_(domain.values.size(), false) {
    add_tracked_variable(variable);
}

bool HandlerFinder::has_any_use() const { return has_any_use_; }

bool HandlerFinder::has_any_check() const { return has_any_check_; }

bool HandlerFinder::forwarded() const { return forwarded_; }

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

bool HandlerFinder::tracks(const clang::VarDecl* variable) const {
    return variable != nullptr && std::find(tracked_variables_.begin(), tracked_variables_.end(),
                                            variable) != tracked_variables_.end();
}

const clang::VarDecl* HandlerFinder::tracked_reference(const clang::Expr* expression) const {
    const clang::VarDecl* variable = referenced_variable(expression);
    return tracks(variable) ? variable : nullptr;
}

const clang::VarDecl*
HandlerFinder::forwarded_tracked_variable(const clang::Expr* expression) const {
    for (const clang::VarDecl* variable : tracked_variables_) {
        if (expression_forwards_variable(expression, variable)) {
            return variable;
        }
    }
    return nullptr;
}

const clang::VarDecl* HandlerFinder::condition_variable(const clang::Stmt* statement) const {
    for (const clang::VarDecl* variable : tracked_variables_) {
        if (expression_references_variable(statement, variable)) {
            return variable;
        }
    }
    return nullptr;
}

bool HandlerFinder::expression_references_tracked(const clang::Stmt* statement) const {
    return condition_variable(statement) != nullptr;
}

void HandlerFinder::add_tracked_variable(const clang::VarDecl* variable) {
    if (variable == nullptr || tracks(variable)) {
        return;
    }
    tracked_variables_.push_back(variable);
}

void HandlerFinder::remove_tracked_variable(const clang::VarDecl* variable) {
    tracked_variables_.erase(
        std::remove(tracked_variables_.begin(), tracked_variables_.end(), variable),
        tracked_variables_.end());
    invalidated_ = tracked_variables_.empty();
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

void HandlerFinder::mark_if_chain(const clang::IfStmt* statement, const clang::VarDecl* variable) {
    has_any_check_ = true;
    const CheckResult result = analyzer_.analyze_if_chain(statement, variable, domain_);

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

void HandlerFinder::mark_condition(const clang::Expr* condition, const clang::VarDecl* variable) {
    has_any_check_ = true;
    const CheckResult result = analyzer_.analyze_condition(condition, variable, domain_);

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
    if (!expression_references_tracked(statement->getCond())) {
        return true;
    }
    mark_switch(statement);
    return true;
}

bool HandlerFinder::VisitIfStmt(clang::IfStmt* statement) {
    if (invalidated_ || !occurs_after(statement->getIfLoc())) {
        return true;
    }
    const clang::VarDecl* variable = condition_variable(statement->getCond());
    if (variable == nullptr) {
        return true;
    }
    mark_if_chain(statement, variable);
    return true;
}

bool HandlerFinder::VisitWhileStmt(clang::WhileStmt* statement) {
    const clang::VarDecl* variable = condition_variable(statement->getCond());
    if (invalidated_ || !occurs_after(statement->getWhileLoc()) || variable == nullptr) {
        return true;
    }
    mark_condition(statement->getCond(), variable);
    return true;
}

bool HandlerFinder::VisitDoStmt(clang::DoStmt* statement) {
    const clang::VarDecl* variable = condition_variable(statement->getCond());
    if (invalidated_ || !occurs_after(statement->getDoLoc()) || variable == nullptr) {
        return true;
    }
    mark_condition(statement->getCond(), variable);
    return true;
}

bool HandlerFinder::VisitForStmt(clang::ForStmt* statement) {
    const clang::VarDecl* variable = condition_variable(statement->getCond());
    if (invalidated_ || !occurs_after(statement->getForLoc()) || statement->getCond() == nullptr ||
        variable == nullptr) {
        return true;
    }
    mark_condition(statement->getCond(), variable);
    return true;
}

bool HandlerFinder::TraverseDeclStmt(clang::DeclStmt* statement, DataRecursionQueue* queue) {
    if (invalidated_ || !occurs_after(statement->getBeginLoc())) {
        return clang::RecursiveASTVisitor<HandlerFinder>::TraverseDeclStmt(statement, queue);
    }

    for (const clang::Decl* declaration : statement->decls()) {
        const auto* variable = llvm::dyn_cast<clang::VarDecl>(declaration);
        if (variable == nullptr || variable->getInit() == nullptr) {
            continue;
        }

        if (forwarded_tracked_variable(variable->getInit()) != nullptr) {
            add_tracked_variable(variable);
            continue;
        }

        if (expression_references_tracked(variable->getInit())) {
            has_any_use_ = true;
            invalidated_ = true;
            return true;
        }
    }
    return true;
}

bool HandlerFinder::TraverseConditionalOperator(clang::ConditionalOperator* expression) {
    const clang::VarDecl* variable = condition_variable(expression->getCond());
    if (invalidated_ || !occurs_after(expression->getQuestionLoc()) || variable == nullptr) {
        return clang::RecursiveASTVisitor<HandlerFinder>::TraverseConditionalOperator(expression);
    }
    mark_condition(expression->getCond(), variable);
    return clang::RecursiveASTVisitor<HandlerFinder>::TraverseConditionalOperator(expression);
}

bool HandlerFinder::TraverseReturnStmt(clang::ReturnStmt* statement) {
    if (invalidated_ || !occurs_after(statement->getReturnLoc())) {
        return clang::RecursiveASTVisitor<HandlerFinder>::TraverseReturnStmt(statement);
    }

    const clang::Expr* value = statement->getRetValue();
    if (forwarded_tracked_variable(value) != nullptr) {
        forwarded_ = true;
        invalidated_ = true;
        return true;
    }

    return clang::RecursiveASTVisitor<HandlerFinder>::TraverseReturnStmt(statement);
}

bool HandlerFinder::VisitBinaryOperator(clang::BinaryOperator* expression) {
    if (invalidated_ || !expression->isAssignmentOp() ||
        !occurs_after(expression->getOperatorLoc())) {
        return true;
    }

    const clang::VarDecl* lhs = referenced_variable(expression->getLHS());
    if (expression->getOpcode() == clang::BO_Assign && lhs != nullptr &&
        forwarded_tracked_variable(expression->getRHS()) != nullptr) {
        add_tracked_variable(lhs);
        return true;
    }

    if (tracks(lhs)) {
        if (expression->getOpcode() != clang::BO_Assign ||
            expression_references_tracked(expression->getRHS())) {
            has_any_use_ = true;
        }
        remove_tracked_variable(lhs);
        return true;
    }

    if (expression_references_tracked(expression->getRHS())) {
        has_any_use_ = true;
        invalidated_ = true;
    }
    return true;
}

bool HandlerFinder::VisitUnaryOperator(clang::UnaryOperator* expression) {
    const clang::VarDecl* variable = tracked_reference(expression->getSubExpr());
    if (invalidated_ || !occurs_after(expression->getOperatorLoc()) || variable == nullptr) {
        return true;
    }

    switch (expression->getOpcode()) {
    case clang::UO_PreInc:
    case clang::UO_PreDec:
    case clang::UO_PostInc:
    case clang::UO_PostDec:
        has_any_use_ = true;
        remove_tracked_variable(variable);
        break;
    default:
        break;
    }
    return true;
}

bool HandlerFinder::VisitDeclRefExpr(clang::DeclRefExpr* reference) {
    if (!invalidated_ && tracks(llvm::dyn_cast<clang::VarDecl>(reference->getDecl())) &&
        occurs_after(reference->getExprLoc())) {
        has_any_use_ = true;
    }
    return true;
}

} // namespace returnguard::internal
