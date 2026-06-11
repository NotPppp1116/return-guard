#pragma once

#include <deque>
#include <unordered_set>

static std::unordered_set<unsigned> blocks_reaching_cfg(const clang::CFG* cfg, const clang::CFGBlock& target) {
    std::unordered_set<unsigned> reachable;
    std::deque<const clang::CFGBlock*> worklist;
    worklist.push_back(&target);

    while (!worklist.empty()) {
        const clang::CFGBlock* block = worklist.front();
        worklist.pop_front();
        if (block == nullptr || !reachable.insert(block->getBlockID()).second) {
            continue;
        }
        for (clang::CFGBlock::const_pred_iterator pred = block->pred_begin(); pred != block->pred_end(); ++pred) {
            if (const clang::CFGBlock* predecessor = *pred) {
                worklist.push_back(predecessor);
            }
        }
    }
    return reachable;
}

static bool var_mutated_in_block(const clang::CFGBlock& block, const clang::VarDecl* target, const clang::ASTContext& context) {
    for (const clang::CFGElement& element : block) {
        if (const auto stmt = element.getAs<clang::CFGStmt>()) {
            const clang::Stmt* s = stmt->getStmt();
            if (const auto* binary = llvm::dyn_cast<clang::BinaryOperator>(s)) {
                if (binary->isAssignmentOp() && referenced_variable(binary->getLHS()) == target) {
                    return true;
                }
            }
            // Walk expressions inside the stmt to find unary ops and calls that touch the var.
            class Finder : public clang::RecursiveASTVisitor<Finder> {
              public:
                Finder(const clang::VarDecl* target, const clang::ASTContext& ctx) : target_(target), ctx_(ctx) {}
                bool VisitUnaryOperator(clang::UnaryOperator* op) {
                    if ((op->getOpcode() == clang::UO_AddrOf || op->isIncrementDecrementOp()) && referenced_variable(op->getSubExpr()) == target_) {
                        found_ = true; return false;
                    }
                    return true;
                }
                bool VisitCallExpr(clang::CallExpr* call) {
                    const clang::FunctionDecl* callee = call->getDirectCallee();
                    for (unsigned i = 0; i < call->getNumArgs(); ++i) {
                        const clang::Expr* arg = strip_expr(call->getArg(i));
                        if (referenced_variable(arg) == target_) {
                            if (callee == nullptr) { found_ = true; return false; }
                            if (i >= callee->getNumParams()) { found_ = true; return false; }
                            const clang::ParmVarDecl* param = callee->getParamDecl(i);
                            if (param->getType()->isPointerType() && !param->getType().isConstQualified()) { found_ = true; return false; }
                        }
                    }
                    return true;
                }
                bool found() const { return found_; }
              private:
                const clang::VarDecl* target_;
                const clang::ASTContext& ctx_;
                bool found_ = false;
            } finder(target, context);
            finder.TraverseStmt(const_cast<clang::Stmt*>(s));
            if (finder.found()) return true;
        }
    }
    return false;
}
