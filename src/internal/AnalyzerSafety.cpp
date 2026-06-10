#include "Analyzer.hpp"
#include "AstUtils.hpp"
#include "NullStateAnalysis.hpp"

#include <returnguard/Options.hpp>

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/Analysis/CFG.h>
#include <clang/Basic/Diagnostic.h>
#include <llvm/Support/Casting.h>

#include <algorithm>
#include <unordered_set>
#include <vector>

namespace returnguard::internal {
namespace {

class SafetyVisitor : public clang::RecursiveASTVisitor<SafetyVisitor> {
  public:
    SafetyVisitor(const clang::FunctionDecl* function, clang::ASTContext& context)
        : function_(function), context_(context) {}

    bool VisitArraySubscriptExpr(clang::ArraySubscriptExpr* subscript) {
        const clang::Expr* base = subscript->getBase()->IgnoreParenCasts();
        const clang::Expr* idx = subscript->getIdx()->IgnoreParenCasts();

        clang::QualType type = base->getType();
        if (const auto* ref = llvm::dyn_cast<clang::DeclRefExpr>(base)) {
            type = ref->getDecl()->getType();
        }

        if (type->isConstantArrayType()) {
            const auto* array_type = llvm::dyn_cast<clang::ConstantArrayType>(type.getTypePtr());
            llvm::APInt size = array_type->getSize();
            auto index_opt = evaluate_integer(idx, context_);
            if (index_opt.has_value()) {
                if (index_opt->isNegative() ||
                    llvm::APSInt::compareValues(*index_opt, llvm::APSInt(size, true)) >= 0) {
                    emit_oob_warning(subscript, *index_opt, size);
                }
            }
        }
        return true;
    }

    bool VisitReturnStmt(clang::ReturnStmt* stmt) {
        const clang::Expr* val = stmt->getRetValue();
        if (val != nullptr && is_stack_address(val)) {
            emit_stack_return_warning(stmt, val);
        }
        return true;
    }

    bool VisitCallExpr(clang::CallExpr* call) {
        const clang::FunctionDecl* callee = call->getDirectCallee();
        if (callee != nullptr && (callee->getName() == "free" || callee->getName() == "realloc")) {
            if (call->getNumArgs() > 0) {
                if (const clang::VarDecl* var = referenced_variable(call->getArg(0))) {
                    check_uaf_for_variable(var, call, call->getArg(0));
                }
            }
        }
        return true;
    }

    bool VisitCXXDeleteExpr(clang::CXXDeleteExpr* delete_expr) {
        if (const clang::VarDecl* var = referenced_variable(delete_expr->getArgument())) {
            check_uaf_for_variable(var, delete_expr, delete_expr->getArgument());
        }
        return true;
    }

  private:
    bool is_stack_address(const clang::Expr* expression) {
        expression = expression->IgnoreParenCasts();
        if (expression == nullptr)
            return false;

        if (const auto* unary = llvm::dyn_cast<clang::UnaryOperator>(expression)) {
            if (unary->getOpcode() == clang::UO_AddrOf) {
                const clang::Expr* sub = unary->getSubExpr()->IgnoreParenCasts();
                if (const auto* ref = llvm::dyn_cast<clang::DeclRefExpr>(sub)) {
                    if (const auto* var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl())) {
                        return var->isLocalVarDecl() && !var->isStaticLocal();
                    }
                }
            }
        }

        if (const auto* ref = llvm::dyn_cast<clang::DeclRefExpr>(expression)) {
            if (const auto* var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl())) {
                if (var->getType()->isArrayType() && var->isLocalVarDecl() &&
                    !var->isStaticLocal()) {
                    return true;
                }
                if (var->isLocalVarDecl() && !var->isStaticLocal() && var->getInit() != nullptr) {
                    return is_stack_address(var->getInit());
                }
            }
        }

        return false;
    }

    void check_uaf_for_variable(const clang::VarDecl* var, const clang::Stmt* free_stmt,
                                const clang::Expr* arg) {
        clang::CFG::BuildOptions options;
        options.PruneTriviallyFalseEdges = true;
        std::unique_ptr<clang::CFG> cfg =
            clang::CFG::buildCFG(function_, function_->getBody(), &context_, options);
        if (!cfg) {
            return;
        }

        // Find the block and index containing free_stmt
        const clang::CFGBlock* start_block = nullptr;
        unsigned start_element_index = 0;
        bool found = false;

        for (const clang::CFGBlock* block : *cfg) {
            if (block == nullptr)
                continue;
            for (unsigned i = 0; i < block->size(); ++i) {
                const clang::CFGElement& element = (*block)[i];
                if (auto cfg_stmt = element.getAs<clang::CFGStmt>()) {
                    if (cfg_stmt->getStmt() == free_stmt) {
                        start_block = block;
                        start_element_index = i;
                        found = true;
                        break;
                    }
                }
            }
            if (found)
                break;
        }

        if (!found)
            return;

        std::vector<std::pair<const clang::CFGBlock*, unsigned>> stack;
        stack.push_back({start_block, start_element_index + 1});

        std::unordered_set<const clang::CFGBlock*> visited;

        while (!stack.empty()) {
            auto [block, element_index] = stack.back();
            stack.pop_back();

            if (block == nullptr)
                continue;

            bool reassigned = false;
            bool used_after_free = false;
            const clang::Stmt* use_stmt = nullptr;

            for (unsigned i = element_index; i < block->size(); ++i) {
                const clang::CFGElement& element = (*block)[i];
                if (auto cfg_stmt = element.getAs<clang::CFGStmt>()) {
                    const clang::Stmt* stmt = cfg_stmt->getStmt();

                    // Treat a variety of mutation patterns as reassignment.
                    if (const auto* binary = llvm::dyn_cast<clang::BinaryOperator>(stmt)) {
                        if (binary->isAssignmentOp() &&
                            referenced_variable(binary->getLHS()) == var) {
                            reassigned = true;
                            break;
                        }
                    }

                    if (const auto* unary = llvm::dyn_cast<clang::UnaryOperator>(stmt)) {
                        switch (unary->getOpcode()) {
                        case clang::UO_PreInc:
                        case clang::UO_PreDec:
                        case clang::UO_PostInc:
                        case clang::UO_PostDec:
                            if (referenced_variable(unary->getSubExpr()) == var) {
                                reassigned = true;
                                break;
                            }
                            break;
                        default:
                            break;
                        }
                        if (reassigned)
                            break;
                    }

                    if (const auto* cxxop = llvm::dyn_cast<clang::CXXOperatorCallExpr>(stmt)) {
                        for (unsigned ai = 0; ai < cxxop->getNumArgs(); ++ai) {
                            if (referenced_variable(cxxop->getArg(ai)) == var) {
                                reassigned = true;
                                break;
                            }
                        }
                        if (reassigned)
                            break;
                    }

                    if (const auto* cxxmem = llvm::dyn_cast<clang::CXXMemberCallExpr>(stmt)) {
                        for (unsigned ai = 0; ai < cxxmem->getNumArgs(); ++ai) {
                            if (referenced_variable(cxxmem->getArg(ai)) == var) {
                                reassigned = true;
                                break;
                            }
                        }
                        if (reassigned)
                            break;
                    }

                    if (const auto* call = llvm::dyn_cast<clang::CallExpr>(stmt)) {
                        for (unsigned ai = 0; ai < call->getNumArgs(); ++ai) {
                            const clang::Expr* call_arg = call->getArg(ai)->IgnoreParenCasts();
                            if (const auto* addr = llvm::dyn_cast<clang::UnaryOperator>(call_arg)) {
                                if (addr->getOpcode() == clang::UO_AddrOf &&
                                    referenced_variable(addr->getSubExpr()) == var) {
                                    reassigned = true;
                                    break;
                                }
                            }
                        }
                        if (reassigned)
                            break;
                    }

                    if (expression_references_variable(stmt, var)) {
                        used_after_free = true;
                        use_stmt = stmt;
                        break;
                    }
                }
            }

            if (reassigned) {
                continue;
            }

            if (used_after_free) {
                emit_uaf_warning(use_stmt, free_stmt, var);
                continue;
            }

            if (!visited.contains(block)) {
                visited.insert(block);
                for (const clang::CFGBlock* succ : block->succs()) {
                    if (succ != nullptr) {
                        stack.push_back({succ, 0});
                    }
                }
            }
        }
    }

    void emit_oob_warning(const clang::ArraySubscriptExpr* expr, const llvm::APSInt& index,
                          const llvm::APInt& size) {
        clang::DiagnosticsEngine& diagnostics = context_.getDiagnostics();
        const clang::DiagnosticsEngine::Level level = returnguard::options().fail_on_diagnostics
                                                          ? clang::DiagnosticsEngine::Error
                                                          : clang::DiagnosticsEngine::Warning;
        const unsigned diagnostic_id = diagnostics.getCustomDiagID(
            level, "returnguard safety: array index %0 is out of bounds (array size is %1)");

        diagnostics.Report(expr->getIdx()->getExprLoc(), diagnostic_id)
            << llvm::toString(index, 10, index.isSigned()) << llvm::toString(size, 10, false);
    }

    void emit_stack_return_warning(const clang::ReturnStmt* stmt, const clang::Expr* val) {
        clang::DiagnosticsEngine& diagnostics = context_.getDiagnostics();
        const clang::DiagnosticsEngine::Level level = returnguard::options().fail_on_diagnostics
                                                          ? clang::DiagnosticsEngine::Error
                                                          : clang::DiagnosticsEngine::Warning;
        const unsigned diagnostic_id = diagnostics.getCustomDiagID(
            level, "returnguard safety: address of stack-allocated variable returned");

        diagnostics.Report(stmt->getReturnLoc(), diagnostic_id);
    }

    void emit_uaf_warning(const clang::Stmt* use_stmt, const clang::Stmt* free_stmt,
                          const clang::VarDecl* var) {
        clang::DiagnosticsEngine& diagnostics = context_.getDiagnostics();
        const clang::DiagnosticsEngine::Level level = returnguard::options().fail_on_diagnostics
                                                          ? clang::DiagnosticsEngine::Error
                                                          : clang::DiagnosticsEngine::Warning;
        const unsigned diagnostic_id = diagnostics.getCustomDiagID(
            level, "returnguard safety: use of memory after it was freed");
        const unsigned note_id = diagnostics.getCustomDiagID(
            clang::DiagnosticsEngine::Note, "returnguard safety: pointer was freed here");

        diagnostics.Report(use_stmt->getBeginLoc(), diagnostic_id);
        diagnostics.Report(free_stmt->getBeginLoc(), note_id);
    }

    const clang::FunctionDecl* function_;
    clang::ASTContext& context_;
};

} // namespace

void Analyzer::analyze_safety(const clang::FunctionDecl* function) {
    if (function == nullptr || function->getBody() == nullptr) {
        return;
    }
    SafetyVisitor visitor(function, context_);
    visitor.TraverseStmt(const_cast<clang::Stmt*>(function->getBody()));
}

} // namespace returnguard::internal
