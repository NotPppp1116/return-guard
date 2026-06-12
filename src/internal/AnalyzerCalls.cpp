#include "Analyzer.hpp"

#include "AstUtils.hpp"
#include "ContractPolicy.hpp"
#include "DirectIf.hpp"
#include "HandlerFinder.hpp"
#include "IfChain.hpp"

#include <returnguard/Options.hpp>

#include <clang/AST/ASTContext.h>
#include <clang/AST/Attr.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/Stmt.h>
#include <clang/Basic/SourceManager.h>
#include <llvm/ADT/APSInt.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Casting.h>

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

bool starts_with_any(llvm::StringRef value, std::initializer_list<llvm::StringRef> prefixes) {
    for (llvm::StringRef prefix : prefixes) {
        if (value.starts_with(prefix)) {
            return true;
        }
    }
    return false;
}

bool is_common_value_helper_name(llvm::StringRef name) {
    static const std::unordered_set<std::string> exact_names = {
        "at",
        "back",
        "begin",
        "c_str",
        "copy",
        "data",
        "distance",
        "empty",
        "end",
        "ERR_CAST",
        "ERR_PTR",
        "extents",
        "front",
        "get",
        "IS_ERR",
        "IS_ERR_OR_NULL",
        "length",
        "lock",
        "makeShared",
        "makeUnique",
        "PTR_ERR",
        "PTR_ERR_OR_ZERO",
        "rc",
        "rbegin",
        "rend",
        "sc",
        "size",
        "str",
        "string",
        "value",
        "valueOrDefault",
        "value_or",
    };
    return exact_names.contains(name.str());
}

bool is_error_pointer_adapter(const clang::FunctionDecl* function) {
    if (function == nullptr || function->getIdentifier() == nullptr) {
        return false;
    }
    static const std::unordered_set<std::string> adapter_names = {
        "ERR_CAST",
        "ERR_PTR",
        "IS_ERR",
        "IS_ERR_OR_NULL",
        "PTR_ERR",
        "PTR_ERR_OR_ZERO",
    };
    return adapter_names.contains(function->getName().str());
}

bool is_common_value_helper(const clang::FunctionDecl* function) {
    if (function == nullptr || function->getIdentifier() == nullptr) {
        return false;
    }
    const llvm::StringRef name = function->getName();
    if (is_common_value_helper_name(name)) {
        return true;
    }
    if (llvm::isa<clang::CXXMethodDecl>(function)) {
        return starts_with_any(name, {
                                         "get",
                                         "is",
                                         "has",
                                         "can",
                                         "should",
                                         "to",
                                     });
    }
    return false;
}

const clang::Expr* peel_equivalent_expr(const clang::Expr* expression) {
    for (unsigned depth = 0; depth < 16U; ++depth) {
        expression = strip_expr(expression);
        if (expression == nullptr) {
            return nullptr;
        }

        if (const auto* cast = llvm::dyn_cast<clang::ExplicitCastExpr>(expression)) {
            expression = cast->getSubExpr();
            continue;
        }
        if (const auto* cleanup = llvm::dyn_cast<clang::ExprWithCleanups>(expression)) {
            expression = cleanup->getSubExpr();
            continue;
        }
        if (const auto* temporary = llvm::dyn_cast<clang::MaterializeTemporaryExpr>(expression)) {
            expression = temporary->getSubExpr();
            continue;
        }
        if (const auto* temporary = llvm::dyn_cast<clang::CXXBindTemporaryExpr>(expression)) {
            expression = temporary->getSubExpr();
            continue;
        }
        if (const auto* unary = llvm::dyn_cast<clang::UnaryOperator>(expression);
            unary != nullptr && unary->getOpcode() == clang::UO_Plus) {
            expression = unary->getSubExpr();
            continue;
        }
        return expression;
    }
    return expression;
}

int compare_to_zero(const llvm::APSInt& value) {
    const llvm::APSInt zero(llvm::APInt(value.getBitWidth(), 0U), value.isUnsigned());
    return llvm::APSInt::compareValues(value, zero);
}

clang::BinaryOperatorKind reversed_comparison(clang::BinaryOperatorKind opcode) {
    switch (opcode) {
    case clang::BO_LT:
        return clang::BO_GT;
    case clang::BO_LE:
        return clang::BO_GE;
    case clang::BO_GT:
        return clang::BO_LT;
    case clang::BO_GE:
        return clang::BO_LE;
    default:
        return opcode;
    }
}

bool is_negative_style_byte_count_check(clang::BinaryOperatorKind opcode,
                                        const llvm::APSInt& constant) {
    const int zero_comparison = compare_to_zero(constant);
    switch (opcode) {
    case clang::BO_LT:
        return zero_comparison >= 0;
    case clang::BO_LE:
        return zero_comparison < 0;
    case clang::BO_EQ:
    case clang::BO_NE:
        return zero_comparison < 0;
    case clang::BO_GE:
        return zero_comparison == 0;
    default:
        return false;
    }
}

bool expressions_equivalent_enough(const clang::Expr* lhs, const clang::Expr* rhs,
                                   const Analyzer& analyzer) {
    lhs = peel_equivalent_expr(lhs);
    rhs = peel_equivalent_expr(rhs);
    if (lhs == nullptr || rhs == nullptr) {
        return false;
    }
    if (lhs == rhs) {
        return true;
    }

    const clang::VarDecl* lhs_variable = referenced_variable(lhs);
    const clang::VarDecl* rhs_variable = referenced_variable(rhs);
    if (lhs_variable != nullptr && lhs_variable == rhs_variable) {
        return true;
    }

    const std::optional<llvm::APSInt> lhs_value = evaluate_integer(lhs, analyzer.context());
    const std::optional<llvm::APSInt> rhs_value = evaluate_integer(rhs, analyzer.context());
    return lhs_value.has_value() && rhs_value.has_value() &&
           llvm::APSInt::compareValues(*lhs_value, *rhs_value) == 0;
}

bool expression_is_byte_count_target(const clang::Expr* expression,
                                     const clang::Expr* target_expression,
                                     const clang::VarDecl* target_variable) {
    expression = peel_equivalent_expr(expression);
    if (expression == nullptr) {
        return false;
    }

    if (target_expression != nullptr && expression == peel_equivalent_expr(target_expression)) {
        return true;
    }

    if (target_variable != nullptr) {
        if (referenced_variable(expression) == target_variable) {
            return true;
        }
        const auto* binary = llvm::dyn_cast<clang::BinaryOperator>(expression);
        if (binary != nullptr && binary->getOpcode() == clang::BO_Assign &&
            referenced_variable(binary->getLHS()) == target_variable) {
            return true;
        }
    }

    return false;
}

struct ByteCountConditionInfo {
    bool compares_with_count = false;
    bool compares_with_negative_failure = false;
};

void update_byte_count_condition_info(const clang::BinaryOperator* binary,
                                      const clang::Expr* target_expression,
                                      const clang::VarDecl* target_variable,
                                      const clang::Expr* count_expression, const Analyzer& analyzer,
                                      ByteCountConditionInfo& info) {
    if (binary == nullptr || !binary->isComparisonOp()) {
        return;
    }

    const clang::Expr* lhs = binary->getLHS();
    const clang::Expr* rhs = binary->getRHS();
    const bool lhs_is_target =
        expression_is_byte_count_target(lhs, target_expression, target_variable);
    const bool rhs_is_target =
        expression_is_byte_count_target(rhs, target_expression, target_variable);
    if (lhs_is_target == rhs_is_target) {
        return;
    }

    const clang::Expr* other = lhs_is_target ? rhs : lhs;
    const clang::BinaryOperatorKind normalized_opcode =
        lhs_is_target ? binary->getOpcode() : reversed_comparison(binary->getOpcode());

    if (expressions_equivalent_enough(other, count_expression, analyzer)) {
        info.compares_with_count = true;
        return;
    }

    const std::optional<llvm::APSInt> constant = evaluate_integer(other, analyzer.context());
    if (constant.has_value() && is_negative_style_byte_count_check(normalized_opcode, *constant)) {
        info.compares_with_negative_failure = true;
    }
}

class ByteCountConditionVisitor final
    : public clang::RecursiveASTVisitor<ByteCountConditionVisitor> {
  public:
    ByteCountConditionVisitor(const clang::Expr* target_expression,
                              const clang::VarDecl* target_variable,
                              const clang::Expr* count_expression, const Analyzer& analyzer)
        : target_expression_(target_expression), target_variable_(target_variable),
          count_expression_(count_expression), analyzer_(analyzer) {}

    bool VisitBinaryOperator(clang::BinaryOperator* binary) {
        update_byte_count_condition_info(binary, target_expression_, target_variable_,
                                         count_expression_, analyzer_, info_);
        return true;
    }

    [[nodiscard]] const ByteCountConditionInfo& info() const { return info_; }

  private:
    const clang::Expr* target_expression_;
    const clang::VarDecl* target_variable_;
    const clang::Expr* count_expression_;
    const Analyzer& analyzer_;
    ByteCountConditionInfo info_;
};

ByteCountConditionInfo inspect_byte_count_condition(const clang::Expr* condition,
                                                    const clang::Expr* target_expression,
                                                    const clang::VarDecl* target_variable,
                                                    const clang::Expr* count_expression,
                                                    const Analyzer& analyzer) {
    ByteCountConditionVisitor visitor(target_expression, target_variable, count_expression,
                                      analyzer);
    visitor.TraverseStmt(const_cast<clang::Expr*>(condition));
    return visitor.info();
}

bool is_read_like_byte_count_function(const clang::FunctionDecl* function) {
    if (function == nullptr || function->getIdentifier() == nullptr) {
        return false;
    }

    static const std::unordered_set<std::string> names = {
        "getrandom",
        "pread",
        "pread64",
        "read",
        "recv",
        "recvfrom",
    };
    return names.contains(function->getName().str());
}

std::string short_transfer_kind(const clang::FunctionDecl* function) {
    if (function == nullptr || function->getIdentifier() == nullptr) {
        return "I/O transfer";
    }

    const llvm::StringRef name = function->getName();
    if (is_read_like_byte_count_function(function) ||
        starts_with_any(name, {"read", "recv", "pread"})) {
        return "read";
    }
    if (starts_with_any(name, {"write", "send", "pwrite"})) {
        return "write";
    }
    return "I/O transfer";
}

CheckResult short_byte_count_result(const clang::CallExpr* call) {
    CheckResult result;
    result.kind = HandlingKind::PartiallyChecked;
    const clang::FunctionDecl* function = call == nullptr ? nullptr : call->getDirectCallee();
    const std::string name =
        function == nullptr ? "<indirect function>" : function->getQualifiedNameAsString();
    const std::string transfer_kind = short_transfer_kind(function);
    const std::string transfer_plural =
        transfer_kind == "read" ? "reads" : (transfer_kind == "write" ? "writes" : "transfers");
    result.message = "possible short " + transfer_kind + " from '" + name +
                     "': positive return may be smaller than requested byte count";
    result.detail =
        "checking only for a negative return misses partial " + transfer_plural +
        "; compare the return value with the requested byte count";
    return result;
}

std::optional<unsigned> byte_count_index_for_call(const clang::CallExpr* call,
                                                  const Analyzer& analyzer) {
    const clang::FunctionDecl* callee = call == nullptr ? nullptr : call->getDirectCallee();
    if (callee == nullptr) {
        return std::nullopt;
    }

    const std::optional<unsigned> index =
        byte_count_parameter_index(*callee, analyzer.source_manager());
    if (!index.has_value() || *index >= call->getNumArgs()) {
        return std::nullopt;
    }
    return index;
}

std::optional<CheckResult> analyze_byte_count_condition(const clang::CallExpr* call,
                                                        const clang::VarDecl* variable,
                                                        const clang::Expr* condition,
                                                        const clang::IfStmt* statement,
                                                        const Analyzer& analyzer) {
    const std::optional<unsigned> index = byte_count_index_for_call(call, analyzer);
    if (!index.has_value() || condition == nullptr) {
        return std::nullopt;
    }

    const ByteCountConditionInfo info =
        inspect_byte_count_condition(condition, call, variable, call->getArg(*index), analyzer);
    if (info.compares_with_count) {
        if (statement == nullptr || has_final_else(statement) ||
            statement_exits(statement->getThen())) {
            return exhaustive_result();
        }
        return std::nullopt;
    }
    if (info.compares_with_negative_failure) {
        return short_byte_count_result(call);
    }
    return std::nullopt;
}

class ByteCountVariableVisitor final : public clang::RecursiveASTVisitor<ByteCountVariableVisitor> {
  public:
    ByteCountVariableVisitor(const clang::CallExpr* call, const clang::VarDecl* variable,
                             const clang::Expr* count_expression, const Analyzer& analyzer)
        : call_(call), variable_(variable), count_expression_(count_expression),
          analyzer_(analyzer) {}

    bool VisitBinaryOperator(clang::BinaryOperator* binary) {
        if (!after_call(binary->getOperatorLoc())) {
            return true;
        }
        update_byte_count_condition_info(binary, nullptr, variable_, count_expression_, analyzer_,
                                         info_);
        return true;
    }

    [[nodiscard]] const ByteCountConditionInfo& info() const { return info_; }

  private:
    [[nodiscard]] bool after_call(clang::SourceLocation location) const {
        const clang::SourceManager& source_manager = analyzer_.source_manager();
        location = source_manager.getFileLoc(location);
        const clang::SourceLocation call_end = source_manager.getFileLoc(call_->getEndLoc());
        return location.isValid() && call_end.isValid() &&
               source_manager.isBeforeInTranslationUnit(call_end, location);
    }

    const clang::CallExpr* call_;
    const clang::VarDecl* variable_;
    const clang::Expr* count_expression_;
    const Analyzer& analyzer_;
    ByteCountConditionInfo info_;
};

std::optional<CheckResult> analyze_byte_count_variable(const clang::CallExpr* call,
                                                       const clang::VarDecl* variable,
                                                       const clang::FunctionDecl* function,
                                                       const Analyzer& analyzer) {
    const std::optional<unsigned> index = byte_count_index_for_call(call, analyzer);
    if (!index.has_value() || variable == nullptr || function == nullptr ||
        !function->doesThisDeclarationHaveABody()) {
        return std::nullopt;
    }

    ByteCountVariableVisitor visitor(call, variable, call->getArg(*index), analyzer);
    visitor.TraverseStmt(const_cast<clang::Stmt*>(function->getBody()));
    const ByteCountConditionInfo& info = visitor.info();
    if (info.compares_with_count) {
        return exhaustive_result();
    }
    if (info.compares_with_negative_failure) {
        return short_byte_count_result(call);
    }
    return std::nullopt;
}

} // namespace

CheckResult Analyzer::analyze_variable(const clang::CallExpr* call, const clang::VarDecl* variable,
                                       const Domain& domain) {
    if (!variable->isLocalVarDecl()) {
        CheckResult result;
        result.kind = HandlingKind::Forwarded;
        return result;
    }

    const clang::FunctionDecl* function = enclosing_function(call);
    if (function == nullptr || !function->doesThisDeclarationHaveABody()) {
        return {
            .kind = HandlingKind::Consumed,
            .missing = {},
            .detail = "result is stored, but no enclosing function body was available",
            .message = {},
        };
    }

    if (std::optional<CheckResult> byte_count =
            analyze_byte_count_variable(call, variable, function, *this)) {
        return *byte_count;
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
            .message = {},
        };
    }

    return {
        .kind = HandlingKind::Ignored,
        .missing = {},
        .detail = "result is stored but never used",
        .message = {},
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
        if (std::optional<CheckResult> byte_count = analyze_byte_count_condition(
                call, nullptr, statement->getCond(), statement, *this)) {
            return *byte_count;
        }
        return analyze_direct_if(statement, call, domain, *this);
    }

    if (const clang::Expr* condition = enclosing_direct_loop_condition(call)) {
        if (std::optional<CheckResult> byte_count =
                analyze_byte_count_condition(call, nullptr, condition, nullptr, *this)) {
            return *byte_count;
        }
        return analyze_direct_condition(condition, call, domain, *this);
    }

    if (const clang::Expr* condition = enclosing_direct_conditional_condition(call)) {
        if (std::optional<CheckResult> byte_count =
                analyze_byte_count_condition(call, nullptr, condition, nullptr, *this)) {
            return *byte_count;
        }
        return exhaustive_result();
    }

    if (const clang::VarDecl* variable = variable_initialized_by_call(call)) {
        CheckResult result = analyze_variable(call, variable, domain);
        if (byte_count_index_for_call(call, *this).has_value() &&
            (result.kind == HandlingKind::ExhaustivelyChecked || !result.message.empty())) {
            return result;
        }
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
                .message = {},
            };
        }
        if (result.kind == HandlingKind::ExhaustivelyChecked ||
            result.kind == HandlingKind::Forwarded) {
            return result;
        }
        return result;
    }

    if (const clang::VarDecl* variable = variable_assigned_from_call(call)) {
        if (const clang::Expr* condition =
                enclosing_assignment_conditional_condition(call, variable)) {
            if (std::optional<CheckResult> byte_count =
                    analyze_byte_count_condition(call, variable, condition, nullptr, *this)) {
                return *byte_count;
            }
            return exhaustive_result();
        }
        if (const clang::Expr* condition = enclosing_assignment_condition(call, variable)) {
            if (std::optional<CheckResult> byte_count =
                    analyze_byte_count_condition(call, variable, condition, nullptr, *this)) {
                return *byte_count;
            }
            return analyze_condition(condition, variable, domain);
        }

        CheckResult result = analyze_variable(call, variable, domain);
        if (byte_count_index_for_call(call, *this).has_value() &&
            (result.kind == HandlingKind::ExhaustivelyChecked || !result.message.empty())) {
            return result;
        }
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
                .message = {},
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
        .message = {},
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
    if (!domain.fallible_contract &&
        (result.kind == HandlingKind::Ignored ||
         result.kind == HandlingKind::Consumed ||
         result.kind == HandlingKind::PartiallyChecked) &&
        is_error_pointer_adapter(call == nullptr ? nullptr : call->getDirectCallee())) {
        return false;
    }

    if (!domain.fallible_contract && !call_requires_verification(call) &&
        (result.kind == HandlingKind::Ignored ||
         result.kind == HandlingKind::Consumed ||
         result.kind == HandlingKind::PartiallyChecked) &&
        is_common_value_helper(call == nullptr ? nullptr : call->getDirectCallee())) {
        return false;
    }

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
        if (domain.fallible_contract &&
            (result.kind == HandlingKind::Consumed ||
             result.kind == HandlingKind::PartiallyChecked)) {
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

    if (!result.message.empty()) {
        message.str({});
        message.clear();
        message << result.message;
    }

    if (const clang::FunctionDecl* enclosing = enclosing_function(call)) {
        message << " in function '" << enclosing->getQualifiedNameAsString() << "'";
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
