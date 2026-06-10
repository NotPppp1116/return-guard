#include "SafetyVisitor.hpp"

#include "Analyzer.hpp"
#include "AstUtils.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/Stmt.h>
#include <clang/Analysis/CFG.h>
#include <llvm/Support/Casting.h>

namespace returnguard::internal {

SafetyVisitor::SafetyVisitor(
    const clang::FunctionDecl* function,
    clang::ASTContext& context)
    : function_(function), context_(context) {
    clang::CFG::BuildOptions options;
    options.PruneTriviallyFalseEdges = true;
    cfg_ = clang::CFG::buildCFG(
        function_,
        function_->getBody(),
        &context_,
        options);
}

SafetyVisitor::~SafetyVisitor() = default;

bool SafetyVisitor::VisitArraySubscriptExpr(
    clang::ArraySubscriptExpr* subscript) {
    maybe_emit_oob_for_array(
        subscript->getBase(),
        subscript->getIdx(),
        subscript);
    return true;
}

bool SafetyVisitor::VisitUnaryOperator(clang::UnaryOperator* unary) {
    if (unary == nullptr || unary->getOpcode() != clang::UO_Deref) {
        return true;
    }

    const clang::Expr* operand =
        unary->getSubExpr()->IgnoreParenCasts();
    const auto* binary =
        llvm::dyn_cast<clang::BinaryOperator>(operand);
    if (binary == nullptr ||
        (binary->getOpcode() != clang::BO_Add &&
         binary->getOpcode() != clang::BO_Sub)) {
        return true;
    }

    maybe_emit_oob_for_array(
        binary->getLHS(), binary->getRHS(), unary);
    maybe_emit_oob_for_array(
        binary->getRHS(), binary->getLHS(), unary);
    return true;
}

bool SafetyVisitor::VisitBinaryOperator(
    clang::BinaryOperator* binary) {
    if (binary == nullptr ||
        (binary->getOpcode() != clang::BO_Shl &&
         binary->getOpcode() != clang::BO_Shr)) {
        return true;
    }

    const std::optional<llvm::APSInt> shift =
        evaluate_integer(binary->getRHS(), context_);
    if (!shift.has_value()) {
        return true;
    }

    const unsigned long long amount = shift->getZExtValue();
    const unsigned long long width =
        context_.getTypeSize(binary->getLHS()->getType());
    if (amount >= width) {
        emit_shift_overflow_warning(binary, amount, width);
        return true;
    }

    if (binary->getOpcode() == clang::BO_Shl &&
        binary->getLHS()->getType()->isSignedIntegerType()) {
        const std::optional<llvm::APSInt> value =
            evaluate_integer(binary->getLHS(), context_);
        if (value.has_value() && !value->isNegative()) {
            const llvm::APInt shifted =
                value->extOrTrunc(static_cast<unsigned>(width))
                    .shl(static_cast<unsigned>(amount));
            if (shifted.isNegative()) {
                emit_signed_shift_overflow_warning(binary);
            }
        }
    }
    return true;
}

bool SafetyVisitor::VisitReturnStmt(clang::ReturnStmt* statement) {
    const clang::Expr* value = statement->getRetValue();
    if (value != nullptr && is_stack_address(value)) {
        emit_stack_return_warning(statement);
    }
    return true;
}

bool SafetyVisitor::VisitCallExpr(clang::CallExpr* call) {
    const clang::FunctionDecl* callee = call->getDirectCallee();
    if (callee == nullptr || callee->getName() != "free" ||
        call->getNumArgs() == 0U) {
        return true;
    }

    if (const clang::VarDecl* variable =
            referenced_variable(call->getArg(0))) {
        check_uaf_for_variable(variable, call);
    }
    return true;
}

bool SafetyVisitor::VisitCXXDeleteExpr(
    clang::CXXDeleteExpr* expression) {
    if (const clang::VarDecl* variable =
            referenced_variable(expression->getArgument())) {
        check_uaf_for_variable(variable, expression);
    }
    return true;
}

void Analyzer::analyze_safety(const clang::FunctionDecl* function) {
    if (function == nullptr || function->getBody() == nullptr) {
        return;
    }
    SafetyVisitor visitor(function, context_);
    visitor.TraverseStmt(
        const_cast<clang::Stmt*>(function->getBody()));
}

} // namespace returnguard::internal
