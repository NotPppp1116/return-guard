#include "Analyzer.hpp"

#include "AstUtils.hpp"
#include "CFGValueFlow.hpp"
#include "DomainUtils.hpp"

#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/Stmt.h>
#include <clang/Basic/SourceManager.h>
#include <llvm/Support/Casting.h>

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace returnguard::internal {
namespace {

bool contains_alias(const clang::Stmt* statement, const ExpressionSet& aliases) {
    if (statement == nullptr) {
        return false;
    }

    if (const auto* expression = llvm::dyn_cast<clang::Expr>(statement)) {
        if (const clang::Expr* canonical = strip_expr(expression);
            canonical != nullptr && aliases.contains(canonical)) {
            return true;
        }
    }

    for (const clang::Stmt* child : statement->children()) {
        if (contains_alias(child, aliases)) {
            return true;
        }
    }
    return false;
}

bool is_alias_expression(const clang::Expr* expression, const ExpressionSet& aliases) {
    const clang::Expr* canonical = strip_expr(expression);
    return canonical != nullptr && aliases.contains(canonical);
}

class ResultAccumulator final {
  public:
    explicit ResultAccumulator(const Domain& domain)
        : domain_(domain), covered_(domain.values.size(), false) {}

    void add(CheckResult result) {
        if (result.kind == HandlingKind::ExhaustivelyChecked) {
            exhaustive_ = true;
            std::fill(covered_.begin(), covered_.end(), true);
            return;
        }
        if (result.kind != HandlingKind::PartiallyChecked) {
            return;
        }

        has_check_ = true;
        if (!result.detail.empty()) {
            detail_ = std::move(result.detail);
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

    void mark_forwarded() { forwarded_ = true; }

    [[nodiscard]] std::optional<CheckResult> result() const {
        if (exhaustive_) {
            CheckResult result;
            result.kind = HandlingKind::ExhaustivelyChecked;
            return result;
        }
        if (forwarded_) {
            CheckResult result;
            result.kind = HandlingKind::Forwarded;
            return result;
        }
        if (!has_check_) {
            return std::nullopt;
        }

        CheckResult result;
        result.kind = HandlingKind::PartiallyChecked;
        result.detail = detail_;
        if (domain_.finite) {
            for (std::size_t index = 0; index < domain_.values.size(); ++index) {
                if (!covered_[index]) {
                    result.missing.push_back(domain_.values[index]);
                }
            }
        }
        return result;
    }

  private:
    const Domain& domain_;
    std::vector<bool> covered_;
    std::string detail_;
    bool has_check_ = false;
    bool exhaustive_ = false;
    bool forwarded_ = false;
};

class FlowHandlingFinder final
    : public clang::RecursiveASTVisitor<FlowHandlingFinder> {
  public:
    FlowHandlingFinder(
        Analyzer& analyzer,
        const clang::CallExpr& call,
        const ExpressionSet& aliases,
        const Domain& domain)
        : analyzer_(analyzer),
          call_(call),
          aliases_(aliases),
          domain_(domain),
          accumulator_(domain) {}

    bool VisitSwitchStmt(clang::SwitchStmt* statement) {
        if (occurs_after(statement->getSwitchLoc()) &&
            contains_alias(statement->getCond(), aliases_)) {
            accumulator_.add(analyzer_.analyze_switch(statement, domain_));
        }
        return true;
    }

    bool VisitIfStmt(clang::IfStmt* statement) {
        if (occurs_after(statement->getIfLoc()) &&
            contains_alias(statement->getCond(), aliases_)) {
            accumulator_.add(analyzer_.analyze_if_chain(statement, aliases_, domain_));
        }
        return true;
    }

    bool VisitWhileStmt(clang::WhileStmt* statement) {
        if (occurs_after(statement->getWhileLoc()) &&
            contains_alias(statement->getCond(), aliases_)) {
            accumulator_.add(analyzer_.analyze_condition(statement->getCond(), aliases_, domain_));
        }
        return true;
    }

    bool VisitDoStmt(clang::DoStmt* statement) {
        if (occurs_after(statement->getDoLoc()) &&
            contains_alias(statement->getCond(), aliases_)) {
            accumulator_.add(analyzer_.analyze_condition(statement->getCond(), aliases_, domain_));
        }
        return true;
    }

    bool VisitForStmt(clang::ForStmt* statement) {
        if (statement->getCond() != nullptr && occurs_after(statement->getForLoc()) &&
            contains_alias(statement->getCond(), aliases_)) {
            accumulator_.add(analyzer_.analyze_condition(statement->getCond(), aliases_, domain_));
        }
        return true;
    }

    bool TraverseConditionalOperator(clang::ConditionalOperator* expression) {
        if (occurs_after(expression->getQuestionLoc()) &&
            contains_alias(expression->getCond(), aliases_)) {
            CheckResult result;
            result.kind = HandlingKind::ExhaustivelyChecked;
            accumulator_.add(std::move(result));
        }
        return clang::RecursiveASTVisitor<FlowHandlingFinder>::TraverseConditionalOperator(
            expression);
    }

    bool TraverseReturnStmt(clang::ReturnStmt* statement) {
        if (occurs_after(statement->getReturnLoc()) &&
            is_alias_expression(statement->getRetValue(), aliases_)) {
            accumulator_.mark_forwarded();
        }
        return clang::RecursiveASTVisitor<FlowHandlingFinder>::TraverseReturnStmt(statement);
    }

    [[nodiscard]] std::optional<CheckResult> result() const {
        return accumulator_.result();
    }

  private:
    [[nodiscard]] bool occurs_after(clang::SourceLocation location) const {
        const clang::SourceManager& manager = analyzer_.source_manager();
        location = manager.getFileLoc(location);
        const clang::SourceLocation after = manager.getFileLoc(call_.getEndLoc());
        if (location.isInvalid() || after.isInvalid() ||
            manager.getFileID(location) != manager.getFileID(after)) {
            return false;
        }
        return manager.isBeforeInTranslationUnit(after, location);
    }

    Analyzer& analyzer_;
    const clang::CallExpr& call_;
    const ExpressionSet& aliases_;
    const Domain& domain_;
    ResultAccumulator accumulator_;
};

} // namespace

CFGValueFlow* Analyzer::value_flow(const clang::FunctionDecl* function) {
    if (function == nullptr || value_flow_failures_.contains(function)) {
        return nullptr;
    }

    if (const auto iterator = value_flow_cache_.find(function);
        iterator != value_flow_cache_.end()) {
        return iterator->second.get();
    }

    std::unique_ptr<CFGValueFlow> flow = CFGValueFlow::build(*function, context_);
    if (!flow) {
        value_flow_failures_.insert(function);
        return nullptr;
    }

    CFGValueFlow* result = flow.get();
    value_flow_cache_.emplace(function, std::move(flow));
    return result;
}

std::optional<CheckResult> Analyzer::analyze_flow_aliases(
    const clang::CallExpr* call,
    const Domain& domain) {
    const clang::FunctionDecl* function = enclosing_function(call);
    CFGValueFlow* flow = value_flow(function);
    if (flow == nullptr) {
        return std::nullopt;
    }

    const ExpressionSet aliases = flow->aliases_for(*call);
    if (aliases.empty()) {
        return std::nullopt;
    }

    FlowHandlingFinder finder(*this, *call, aliases, domain);
    finder.TraverseStmt(const_cast<clang::Stmt*>(function->getBody()));
    return finder.result();
}

} // namespace returnguard::internal
