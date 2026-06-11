#include "Analyzer.hpp"

#include "DirectIf.hpp"
#include "HandlerFinder.hpp"
#include "IfChain.hpp"

#include <returnguard/Options.hpp>

#include <clang/AST/Attr.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Expr.h>
#include <clang/AST/Stmt.h>
#include <clang/Basic/SourceManager.h>
#include <llvm/ADT/StringRef.h>

#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace returnguard::internal {
namespace {

CheckResult exhaustive_result() {
    CheckResult result;
    result.kind = HandlingKind::ExhaustivelyChecked;
    return result;
}

bool is_commonly_ignored_system_function(const clang::FunctionDecl* function) {
    if (function == nullptr) {
        return false;
    }

    bool in_std_namespace = false;
    bool in_testing_namespace = false;
    for (const clang::DeclContext* context = function->getDeclContext(); context != nullptr;
         context = context->getParent()) {
        const auto* namespace_declaration = llvm::dyn_cast<clang::NamespaceDecl>(context);
        if (namespace_declaration == nullptr) {
            continue;
        }
        if (namespace_declaration->getName() == "std") {
            in_std_namespace = true;
        }
        if (namespace_declaration->getName() == "testing") {
            in_testing_namespace = true;
        }
    }

    if (in_testing_namespace) {
        return true;
    }
    if (in_std_namespace && llvm::isa<clang::CXXConversionDecl>(function)) {
        return true;
    }

    if (function->getIdentifier() == nullptr) {
        return false;
    }
    const llvm::StringRef name = function->getName();
    static const std::unordered_set<std::string> ignored_names = {
        "__builtin_expect",
        "__builtin_expect_with_probability",
        "printf",
        "fprintf",
        "sprintf",
        "snprintf",
        "vprintf",
        "vfprintf",
        "vsprintf",
        "vsnprintf",
        "memcpy",
        "memmove",
        "memset",
        "strcpy",
        "strncpy",
        "strcat",
        "strncat",
        "putchar",
        "putc",
        "puts",
        "fwrite"};
    if (ignored_names.contains(name.str())) {
        return true;
    }

    static const std::unordered_set<std::string> ignored_wayland_names = {
        "wl_client_get_object",
        "wl_display_get_event_loop",
        "wl_display_get_fd",
        "wl_display_get_registry",
        "wl_display_next_serial",
        "wl_fixed_from_double",
        "wl_fixed_from_int",
        "wl_fixed_to_double",
        "wl_proxy_get_version",
        "wl_resource_get_user_data",
        "wl_resource_get_version",
    };
    if (ignored_wayland_names.contains(name.str())) {
        return true;
    }

    if (!in_std_namespace) {
        return false;
    }

    static const std::unordered_set<std::string> ignored_std_names = {
        "abs",
        "any_cast",
        "back_inserter",
        "begin",
        "ceil",
        "canonical",
        "count",
        "create_directory",
        "distance",
        "duration_cast",
        "empty",
        "size",
        "length",
        "data",
        "c_str",
        "substr",
        "find",
        "rfind",
        "find_first_of",
        "find_first_not_of",
        "find_last_of",
        "find_last_not_of",
        "starts_with",
        "ends_with",
        "contains",
        "compare",
        "emplace",
        "end",
        "exists",
        "first",
        "for_each",
        "str",
        "format",
        "has_value",
        "insert",
        "insert_range",
        "make_pair",
        "make_move_iterator",
        "make_tuple",
        "max",
        "next",
        "permissions",
        "pow",
        "prev",
        "ref",
        "remove",
        "remove_all",
        "rbegin",
        "rend",
        "round",
        "rotate",
        "status",
        "string",
        "time_point_cast",
        "to",
        "to_array",
        "to_string",
        "stoi",
        "stol",
        "stoll",
        "stoul",
        "stoull",
        "stof",
        "stod",
        "stold",
        "erase",
        "erase_if",
        "value_or",
        "what",
    };
    return ignored_std_names.contains(name.str());
}

bool is_boolean_domain(const Domain& domain) {
    return domain.finite && domain.values.size() == 2U &&
           (domain.type_name == "bool" || domain.type_name == "_Bool");
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
        if (std::optional<CheckResult> flow = analyze_flow_aliases(call, domain)) {
            return *flow;
        }
        if ((result.kind == HandlingKind::ExhaustivelyChecked ||
             result.kind == HandlingKind::Forwarded) &&
            value_flow(enclosing_function(call)) != nullptr) {
            return {
                .kind = HandlingKind::Consumed,
                .missing = {},
                .detail = "stored result has no reachable CFG-proven handling",
            };
        }
        if (result.kind == HandlingKind::ExhaustivelyChecked ||
            result.kind == HandlingKind::Forwarded) {
            return result;
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
        if (std::optional<CheckResult> flow = analyze_flow_aliases(call, domain)) {
            return *flow;
        }
        if ((result.kind == HandlingKind::ExhaustivelyChecked ||
             result.kind == HandlingKind::Forwarded) &&
            value_flow(enclosing_function(call)) != nullptr) {
            return {
                .kind = HandlingKind::Consumed,
                .missing = {},
                .detail = "stored result has no reachable CFG-proven handling",
            };
        }
        if (result.kind == HandlingKind::ExhaustivelyChecked ||
            result.kind == HandlingKind::Forwarded) {
            return result;
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

bool Analyzer::call_requires_verification(const clang::CallExpr* call) const {
    const clang::FunctionDecl* callee = call == nullptr ? nullptr : call->getDirectCallee();
    if (callee == nullptr) {
        return false;
    }

    for (const clang::FunctionDecl* redeclaration : callee->redecls()) {
        if (redeclaration->hasAttr<clang::WarnUnusedResultAttr>()) {
            return true;
        }
    }
    return false;
}

bool Analyzer::should_report(const CheckResult& result, const Domain& domain,
                             const clang::CallExpr* call) const {
    switch (returnguard::options().mode) {
    case Mode::IgnoredOnly:
        return result.kind == HandlingKind::Ignored;

    case Mode::Practical:
        if (result.kind == HandlingKind::Ignored) {
            return true;
        }
        if (call_requires_verification(call) && result.kind == HandlingKind::Consumed) {
            return true;
        }
        if (is_boolean_domain(domain) && result.kind == HandlingKind::PartiallyChecked) {
            return false;
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

    if (const clang::FunctionDecl* callee = call->getDirectCallee()) {
        if ((source_manager_.isInSystemHeader(callee->getLocation()) ||
             callee->getBuiltinID() != 0U) &&
            is_commonly_ignored_system_function(callee)) {
            return;
        }
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
    if (!should_report(result, domain, call)) {
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

} // namespace returnguard::internal
