#include "Analyzer.hpp"

#include "DirectIf.hpp"
#include "HandlerFinder.hpp"
#include "IfChain.hpp"

#include <returnguard/Options.hpp>

#include <clang/AST/ASTContext.h>
#include <clang/AST/Attr.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Expr.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/Type.h>
#include <clang/Basic/SourceManager.h>

#include <algorithm>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace returnguard::internal {
namespace {

CheckResult exhaustive_result() {
    CheckResult result;
    result.kind = HandlingKind::ExhaustivelyChecked;
    return result;
}

bool declaration_requires_a_used_result(const clang::FunctionDecl& function) {
    for (const clang::FunctionDecl* redeclaration : function.redecls()) {
        if (redeclaration->hasAttr<clang::WarnUnusedResultAttr>()) {
            return true;
        }
    }

    const clang::QualType return_type = function.getReturnType();
    if (const auto* record = return_type->getAsCXXRecordDecl()) {
        return record->hasAttr<clang::WarnUnusedResultAttr>();
    }
    return false;
}

} // namespace

CheckResult Analyzer::analyze_variable(const clang::CallExpr* call, const clang::VarDecl* variable,
                                       const Domain& domain) {
    const clang::FunctionDecl* function = enclosing_function(call);
    if (function == nullptr || !function->doesThisDeclarationHaveABody()) {
        return {
            .kind = HandlingKind::Consumed,
            .missing = {},
            .detail = "result is stored, but no enclosing function body was available",
        };
    }

    HandlerFinder finder(*this, variable, call->getEndLoc(), domain);
    finder.TraverseStmt(const_cast<clang::Stmt*>(function->getBody()));

    if (finder.exhaustive()) {
        return exhaustive_result();
    }

    if (finder.forwarded()) {
        CheckResult result;
        result.kind = HandlingKind::Forwarded;
        return result;
    }

    if (finder.has_any_check()) {
        CheckResult result;
        result.kind = HandlingKind::PartiallyChecked;
        if (domain.finite) {
            const std::vector<bool>& covered = finder.covered();
            for (std::size_t index = 0; index < domain.values.size(); ++index) {
                if (!covered[index]) {
                    result.missing.push_back(domain.values[index]);
                }
            }
        } else {
            result.detail = "checks do not have a final else/default";
        }
        return result;
    }

    if (finder.has_any_use()) {
        return {
            .kind = HandlingKind::Consumed,
            .missing = {},
            .detail = "result is used but never checked",
        };
    }

    return {
        .kind = HandlingKind::Ignored,
        .missing = {},
        .detail = "result is stored but never used",
    };
}

CheckResult Analyzer::classify_call(const clang::CallExpr* call, const Domain& domain) {
    if (returnguard::options().explicit_void_is_handled && is_explicit_void_discard(call)) {
        CheckResult result;
        result.kind = HandlingKind::ExplicitlyIgnored;
        return result;
    }

    if (const clang::SwitchStmt* statement = enclosing_direct_switch(call)) {
        return analyze_switch(statement, domain);
    }

    if (const clang::IfStmt* statement = enclosing_direct_if(call)) {
        return analyze_direct_if(statement, call, domain, *this);
    }

    if (const clang::Expr* condition = enclosing_direct_loop_condition(call)) {
        return analyze_direct_condition(condition, call, domain, *this);
    }

    if (enclosing_direct_conditional_condition(call) != nullptr) {
        return exhaustive_result();
    }

    if (const clang::VarDecl* variable = variable_initialized_by_call(call)) {
        CheckResult result = analyze_variable(call, variable, domain);
        if (result.kind == HandlingKind::ExhaustivelyChecked ||
            result.kind == HandlingKind::Forwarded) {
            return result;
        }
        if (std::optional<CheckResult> flow = analyze_flow_aliases(call, domain)) {
            return *flow;
        }
        return result;
    }

    if (const clang::VarDecl* variable = variable_assigned_from_call(call)) {
        if (enclosing_assignment_conditional_condition(call, variable) != nullptr) {
            return exhaustive_result();
        }
        if (const clang::Expr* condition = enclosing_assignment_condition(call, variable)) {
            return analyze_condition(condition, variable, domain);
        }

        CheckResult result = analyze_variable(call, variable, domain);
        if (result.kind == HandlingKind::ExhaustivelyChecked ||
            result.kind == HandlingKind::Forwarded) {
            return result;
        }
        if (std::optional<CheckResult> flow = analyze_flow_aliases(call, domain)) {
            return *flow;
        }
        return result;
    }

    if (call_is_forwarded(call)) {
        CheckResult result;
        result.kind = HandlingKind::Forwarded;
        return result;
    }

    if (std::optional<CheckResult> flow = analyze_flow_aliases(call, domain)) {
        return *flow;
    }

    if (call_is_discarded_expression(call)) {
        CheckResult result;
        result.kind = HandlingKind::Ignored;
        return result;
    }

    return {
        .kind = HandlingKind::Consumed,
        .missing = {},
        .detail = "result participates in an expression but is not checked",
    };
}

bool Analyzer::should_report(const CheckResult& result, const Domain& domain) const {
    switch (returnguard::options().mode) {
    case Mode::IgnoredOnly:
        return result.kind == HandlingKind::Ignored;

    case Mode::Practical:
        if (result.kind == HandlingKind::Ignored) {
            return true;
        }
        return domain.finite && result.kind == HandlingKind::PartiallyChecked;

    case Mode::Strict:
        return result.kind != HandlingKind::ExhaustivelyChecked &&
               result.kind != HandlingKind::ExplicitlyIgnored &&
               result.kind != HandlingKind::Forwarded;
    }
    return false;
}

void Analyzer::analyze_call(clang::CallExpr* call) {
    if (!should_analyze_location(call->getExprLoc())) {
        return;
    }

    if (!returnguard::options().include_operators && call_is_operator(call)) {
        return;
    }

    clang::QualType return_type = call->getCallReturnType(context_);
    if (return_type->isVoidType() || return_type->isDependentType()) {
        return;
    }

    if (!returnguard::options().include_reference_returns && return_type->isReferenceType()) {
        return;
    }

    const Domain domain = call_domain(call);
    const CheckResult result = classify_call(call, domain);

    // In practical policies, defer to Clang's declaration-level contract for
    // discarded calls from system headers. Unlike the old name allowlist, this
    // still analyzes stored/checked results and honors [[nodiscard]] and
    // warn_unused_result on any API.
    if (result.kind == HandlingKind::Ignored && returnguard::options().mode != Mode::Strict) {
        if (const clang::FunctionDecl* callee = call->getDirectCallee();
            callee != nullptr && source_manager_.isInSystemHeader(callee->getLocation()) &&
            !declaration_requires_a_used_result(*callee)) {
            return;
        }
    }

    if (!should_report(result, domain)) {
        return;
    }

    const std::string name = function_name(call);
    std::ostringstream message;

    switch (result.kind) {
    case HandlingKind::Ignored:
        message << "return value of '" << name << "' (" << return_type.getAsString()
                << ") is not handled";
        break;
    case HandlingKind::Consumed:
        message << "return value of '" << name << "' is consumed but not verified";
        break;
    case HandlingKind::PartiallyChecked:
        message << "return value of '" << name << "' is not handled exhaustively";
        break;
    case HandlingKind::ExplicitlyIgnored:
    case HandlingKind::Forwarded:
    case HandlingKind::ExhaustivelyChecked:
        return;
    }

    std::string note;
    if (!result.missing.empty()) {
        note = missing_message(result.missing);
    } else if (!result.detail.empty()) {
        note = result.detail;
    } else if (!domain.finite) {
        note =
            "return domain is open-ended; use a final else/default or an explicit (void) discard";
    }

    emit(call, message.str(), note);
}

bool Analyzer::function_checks_parameter(
    const clang::FunctionDecl* function,
    unsigned param_index,
    const Domain& domain) const {
    if (function == nullptr) {
        return false;
    }
    const clang::FunctionDecl* canonical = function->getCanonicalDecl();

    auto target = std::make_pair(canonical, param_index);
    if (std::find(active_parameter_checks_.begin(), active_parameter_checks_.end(), target) !=
        active_parameter_checks_.end()) {
        return false;
    }
    active_parameter_checks_.push_back(target);

    const clang::FunctionDecl* definition = nullptr;
    if (canonical->hasBody(definition) && definition != nullptr &&
        param_index < definition->getNumParams()) {
        const clang::ParmVarDecl* param = definition->getParamDecl(param_index);
        HandlerFinder finder(const_cast<Analyzer&>(*this), param,
                             definition->getBody()->getBeginLoc(), domain);
        finder.TraverseStmt(const_cast<clang::Stmt*>(definition->getBody()));

        active_parameter_checks_.erase(
            std::remove(active_parameter_checks_.begin(), active_parameter_checks_.end(), target),
            active_parameter_checks_.end());
        return finder.exhaustive();
    }

    active_parameter_checks_.erase(
        std::remove(active_parameter_checks_.begin(), active_parameter_checks_.end(), target),
        active_parameter_checks_.end());
    return false;
}

} // namespace returnguard::internal
