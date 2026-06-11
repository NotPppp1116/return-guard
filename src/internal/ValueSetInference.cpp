#include "ValueSetInference.hpp"

#include "Analyzer.hpp"
#include "AstUtils.hpp"
#include "DomainUtils.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/Stmt.h>
#include <clang/Analysis/CFG.h>
#include <clang/Basic/SourceManager.h>
#include <llvm/ADT/APSInt.h>
#include <llvm/Support/Casting.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <unordered_set>
#include <vector>

namespace returnguard::internal {

ValueSetInference::ValueSetInference(Analyzer& analyzer)
    : analyzer_(analyzer), context_(analyzer.context()) {}

std::optional<Domain> ValueSetInference::infer_expression(
    const clang::Expr* expression, std::unordered_set<const clang::FunctionDecl*>& active_functions,
    std::unordered_set<const clang::VarDecl*>& active_variables) {
    expression = strip_expr(expression);
    if (expression == nullptr) {
        return std::nullopt;
    }

    if (const std::optional<llvm::APSInt> value = evaluate_integer(expression, context_)) {
        Domain domain;
        domain.finite = true;
        domain.inferred_from_body = true;
        domain.type_name = expression->getType().getAsString();
        std::string label = "";
        if (const auto* reference = llvm::dyn_cast<clang::DeclRefExpr>(expression)) {
            if (const auto* enum_constant = llvm::dyn_cast<clang::EnumConstantDecl>(reference->getDecl())) {
                label = enum_constant->getQualifiedNameAsString();
            }
        }
        add_domain_value(domain, *value, label);
        return domain;
    }

    if (const auto* reference = llvm::dyn_cast<clang::DeclRefExpr>(expression)) {
        if (const auto* variable = llvm::dyn_cast<clang::VarDecl>(reference->getDecl())) {
            return infer_variable(variable, reference, active_functions, active_variables);
        }
    }

    if (const auto* binary = llvm::dyn_cast<clang::BinaryOperator>(expression);
        binary != nullptr && binary->getOpcode() == clang::BO_Comma) {
        return infer_expression(binary->getRHS(), active_functions, active_variables);
    }

    if (const auto* conditional = llvm::dyn_cast<clang::ConditionalOperator>(expression)) {
        auto true_domain =
            infer_expression(conditional->getTrueExpr(), active_functions, active_variables);
        auto false_domain =
            infer_expression(conditional->getFalseExpr(), active_functions, active_variables);
        if (true_domain && true_domain->finite && false_domain && false_domain->finite) {
            Domain combined = *true_domain;
            for (const auto& val : false_domain->values) {
                for (const auto& label : val.labels) {
                    add_domain_value(combined, val.value, label);
                }
                if (val.labels.empty()) {
                    add_domain_value(combined, val.value, "");
                }
            }
            combined.inferred_from_body = true;
            return combined;
        }
    }

    if (const auto* call = llvm::dyn_cast<clang::CallExpr>(expression)) {
        if (const auto* callee = call->getDirectCallee()) {
            Domain d = analyzer_.function_domain(callee, active_functions);
            if (d.finite) {
                return d;
            }
        } else {
            const clang::Expr* callee_expr = strip_expr(call->getCallee());
            if (const auto* ref = llvm::dyn_cast<clang::DeclRefExpr>(callee_expr)) {
                if (const auto* param = llvm::dyn_cast<clang::ParmVarDecl>(ref->getDecl())) {
                    const auto* function = llvm::dyn_cast<clang::FunctionDecl>(param->getDeclContext());
                    if (function) {
                        unsigned param_index = param->getFunctionScopeIndex();
                        class CallSiteFinder : public clang::RecursiveASTVisitor<CallSiteFinder> {
                          public:
                            CallSiteFinder(const clang::FunctionDecl* target) : target_(target->getCanonicalDecl()) {}
                            bool VisitCallExpr(clang::CallExpr* c) {
                                if (const clang::FunctionDecl* callee = c->getDirectCallee()) {
                                    if (callee->getCanonicalDecl() == target_) {
                                        calls_.push_back(c);
                                    }
                                }
                                return true;
                            }
                            const clang::FunctionDecl* target_;
                            std::vector<const clang::CallExpr*> calls_;
                        };
                        CallSiteFinder finder(function);
                        finder.TraverseDecl(context_.getTranslationUnitDecl());
                        
                        if (!finder.calls_.empty()) {
                            Domain combined;
                            combined.finite = true;
                            combined.inferred_from_body = true;
                            combined.type_name = call->getType().getAsString();
                            for (const clang::CallExpr* c : finder.calls_) {
                                if (param_index >= c->getNumArgs()) continue;
                                const clang::Expr* arg = strip_expr(c->getArg(param_index));
                                if (const auto* arg_unary = llvm::dyn_cast<clang::UnaryOperator>(arg)) {
                                    if (arg_unary->getOpcode() == clang::UO_AddrOf) {
                                        arg = strip_expr(arg_unary->getSubExpr());
                                    }
                                }
                                if (const auto* arg_ref = llvm::dyn_cast<clang::DeclRefExpr>(arg)) {
                                    if (const auto* passed_func = llvm::dyn_cast<clang::FunctionDecl>(arg_ref->getDecl())) {
                                        Domain d = analyzer_.function_domain(passed_func, active_functions);
                                        if (!d.finite) {
                                            combined.finite = false;
                                            break;
                                        }
                                        for (const auto& val : d.values) {
                                            for (const auto& label : val.labels) {
                                                add_domain_value(combined, val.value, label);
                                            }
                                            if (val.labels.empty()) {
                                                add_domain_value(combined, val.value, "");
                                            }
                                        }
                                    } else {
                                        combined.finite = false;
                                        break;
                                    }
                                } else {
                                    combined.finite = false;
                                    break;
                                }
                            }
                            if (combined.finite) return combined;
                        }
                    }
                }
            }
        }
    }

    return std::nullopt;
}

namespace {
bool contains_statement(const clang::Stmt* root, const clang::Stmt* needle) {
    if (root == nullptr || needle == nullptr) {
        return false;
    }
    if (root == needle) {
        return true;
    }
    for (const clang::Stmt* child : root->children()) {
        if (contains_statement(child, needle)) {
            return true;
        }
    }
    return false;
}

class SimpleAssignmentFinder : public clang::RecursiveASTVisitor<SimpleAssignmentFinder> {
  public:
    SimpleAssignmentFinder(const clang::VarDecl* target, const clang::Expr* reference_site,
                           const clang::ASTContext& context)
        : target_(target), reference_site_(reference_site), context_(context) {}

    bool VisitUnaryOperator(clang::UnaryOperator* op) {
        if (op->getOpcode() == clang::UO_AddrOf &&
            referenced_variable(op->getSubExpr()) == target_ && occurs_before_reference(op)) {
            valid_ = false;
            return false;
        }
        if (op->isIncrementDecrementOp() && referenced_variable(op->getSubExpr()) == target_ &&
            occurs_before_reference(op)) {
            valid_ = false;
            return false;
        }
        return true;
    }

    bool VisitBinaryOperator(clang::BinaryOperator* op) {
        if (!op->isAssignmentOp() || referenced_variable(op->getLHS()) != target_ ||
            !occurs_before_reference(op)) {
            return true;
        }

        if (op->getOpcode() != clang::BO_Assign) {
            valid_ = false;
            return false;
        }
        assignments_.push_back(op->getRHS());
        return true;
    }

    bool add_initializer_if_relevant() {
        if (!target_->hasInit()) {
            return true;
        }
        if (!occurs_before_reference(target_)) {
            valid_ = false;
            return false;
        }
        assignments_.push_back(target_->getInit());
        return true;
    }

    bool valid_ = true;
    std::vector<const clang::Expr*> assignments_;

  private:
    bool occurs_before_reference(const clang::Stmt* statement) const {
        return occurs_before_reference(statement == nullptr ? clang::SourceLocation{}
                                                            : statement->getBeginLoc());
    }

    bool occurs_before_reference(const clang::Decl* declaration) const {
        return occurs_before_reference(declaration == nullptr ? clang::SourceLocation{}
                                                              : declaration->getLocation());
    }

    bool occurs_before_reference(clang::SourceLocation location) const {
        const clang::SourceManager& source_manager = context_.getSourceManager();
        location = source_manager.getFileLoc(location);
        clang::SourceLocation reference = source_manager.getFileLoc(reference_site_->getExprLoc());
        if (location.isInvalid() || reference.isInvalid() ||
            source_manager.getFileID(location) != source_manager.getFileID(reference)) {
            return false;
        }
        return source_manager.isBeforeInTranslationUnit(location, reference);
    }

    const clang::VarDecl* target_;
    const clang::Expr* reference_site_;
    const clang::ASTContext& context_;
};

const clang::CFGBlock* block_containing_reference(const clang::CFG& cfg,
                                                  const clang::Expr* reference_site) {
    for (const clang::CFGBlock* block : cfg) {
        for (const clang::CFGElement& element : *block) {
            const auto statement = element.getAs<clang::CFGStmt>();
            if (!statement.has_value()) {
                continue;
            }
            if (contains_statement(statement->getStmt(), reference_site)) {
                return block;
            }
        }
    }
    return nullptr;
}

std::unordered_set<unsigned> blocks_reaching(const clang::CFGBlock& target) {
    std::unordered_set<unsigned> reachable;
    std::deque<const clang::CFGBlock*> worklist;
    worklist.push_back(&target);

    while (!worklist.empty()) {
        const clang::CFGBlock* block = worklist.front();
        worklist.pop_front();
        if (block == nullptr || !reachable.insert(block->getBlockID()).second) {
            continue;
        }
        for (clang::CFGBlock::const_pred_iterator pred = block->pred_begin();
             pred != block->pred_end(); ++pred) {
            if (const clang::CFGBlock* predecessor = *pred) {
                worklist.push_back(predecessor);
            }
        }
    }

    return reachable;
}
} // namespace

std::optional<Domain>
ValueSetInference::infer_variable(const clang::VarDecl* variable, const clang::Expr* reference_site,
                                  std::unordered_set<const clang::FunctionDecl*>& active_functions,
                                  std::unordered_set<const clang::VarDecl*>& active_variables) {
    if (llvm::isa<clang::ParmVarDecl>(variable)) {
        const auto* param = llvm::dyn_cast<clang::ParmVarDecl>(variable);
        const auto* function = llvm::dyn_cast<clang::FunctionDecl>(param->getDeclContext());
        if (!function) return std::nullopt;
        
        unsigned param_index = param->getFunctionScopeIndex();
        
        class CallSiteFinder : public clang::RecursiveASTVisitor<CallSiteFinder> {
          public:
            CallSiteFinder(const clang::FunctionDecl* target) : target_(target->getCanonicalDecl()) {}
            bool VisitCallExpr(clang::CallExpr* c) {
                if (const clang::FunctionDecl* callee = c->getDirectCallee()) {
                    if (callee->getCanonicalDecl() == target_) {
                        calls_.push_back(c);
                    }
                }
                return true;
            }
            const clang::FunctionDecl* target_;
            std::vector<const clang::CallExpr*> calls_;
        };
        
        CallSiteFinder finder(function);
        finder.TraverseDecl(context_.getTranslationUnitDecl());
        
        if (finder.calls_.empty()) {
            return std::nullopt;
        }
        
        Domain combined;
        combined.finite = true;
        combined.inferred_from_body = true;
        combined.type_name = variable->getType().getAsString();
        
        for (const clang::CallExpr* call : finder.calls_) {
            if (param_index >= call->getNumArgs()) continue;
            const clang::Expr* arg = call->getArg(param_index);
            auto part = infer_expression(arg, active_functions, active_variables);
            if (!part || !part->finite) {
                combined.finite = false;
                break;
            }
            for (const auto& val : part->values) {
                for (const auto& label : val.labels) {
                    add_domain_value(combined, val.value, label);
                }
                if (val.labels.empty()) {
                    add_domain_value(combined, val.value, "");
                }
            }
        }
        if (combined.finite) return combined;
        return std::nullopt;
    }

    if (active_variables.contains(variable)) {
        return std::nullopt;
    }
    active_variables.insert(variable);

    const clang::DeclContext* dc = variable->getDeclContext();
    const auto* function = llvm::dyn_cast_or_null<clang::FunctionDecl>(dc);

    std::vector<const clang::Expr*> assignments;
    bool valid = true;

    if (!variable->isLocalVarDecl()) {
        class GlobalAssignmentFinder : public clang::RecursiveASTVisitor<GlobalAssignmentFinder> {
          public:
            GlobalAssignmentFinder(const clang::VarDecl* target) : target_(target) {}

            bool VisitBinaryOperator(clang::BinaryOperator* op) {
                if (op->isAssignmentOp() && referenced_variable(op->getLHS()) == target_) {
                    if (op->getOpcode() != clang::BO_Assign) {
                        valid_ = false;
                        return false;
                    }
                    assignments_.push_back(op->getRHS());
                }
                return true;
            }

            bool VisitUnaryOperator(clang::UnaryOperator* op) {
                if ((op->getOpcode() == clang::UO_AddrOf || op->isIncrementDecrementOp()) &&
                    referenced_variable(op->getSubExpr()) == target_) {
                    valid_ = false;
                    return false;
                }
                return true;
            }

            bool valid_ = true;
            std::vector<const clang::Expr*> assignments_;
            
          private:
            const clang::VarDecl* target_;
        };

        GlobalAssignmentFinder global_finder(variable);
        global_finder.TraverseDecl(context_.getTranslationUnitDecl());
        if (variable->hasInit()) {
            global_finder.assignments_.push_back(variable->getInit());
        }

        valid = global_finder.valid_;
        assignments = std::move(global_finder.assignments_);
    } else {
        if (!function || !function->hasBody()) {
            active_variables.erase(variable);
            return std::nullopt;
        }

        clang::CFG::BuildOptions options;
        std::unique_ptr<clang::CFG> cfg = clang::CFG::buildCFG(
            function, function->getBody(), const_cast<clang::ASTContext*>(&context_), options);
        if (!cfg) {
            active_variables.erase(variable);
            return std::nullopt;
        }

        const clang::CFGBlock* reference_block = block_containing_reference(*cfg, reference_site);
        if (reference_block == nullptr) {
            active_variables.erase(variable);
            return std::nullopt;
        }

        const std::unordered_set<unsigned> reaching_blocks = blocks_reaching(*reference_block);

        SimpleAssignmentFinder finder(variable, reference_site, context_);
        finder.add_initializer_if_relevant();

        for (const clang::CFGBlock* block : *cfg) {
            if (block == nullptr || !reaching_blocks.contains(block->getBlockID())) {
                continue;
            }
            for (const clang::CFGElement& element : *block) {
                const auto statement = element.getAs<clang::CFGStmt>();
                if (!statement.has_value()) {
                    continue;
                }
                finder.TraverseStmt(const_cast<clang::Stmt*>(statement->getStmt()));
                if (!finder.valid_) {
                    break;
                }
            }
            if (!finder.valid_) {
                break;
            }
        }
        valid = finder.valid_;
        assignments = std::move(finder.assignments_);
    }

    if (!valid || assignments.empty()) {
        active_variables.erase(variable);
        return std::nullopt;
    }

    Domain combined;
    combined.finite = true;
    combined.inferred_from_body = true;
    combined.type_name = variable->getType().getAsString();

    for (const auto* assignment : assignments) {
        auto part = infer_expression(assignment, active_functions, active_variables);
        if (!part || !part->finite) {
            combined.finite = false;
            break;
        }
        for (const auto& val : part->values) {
            for (const auto& label : val.labels) {
                add_domain_value(combined, val.value, label);
            }
            if (val.labels.empty()) {
                add_domain_value(combined, val.value, "");
            }
        }
    }

    active_variables.erase(variable);
    if (combined.finite)
        return combined;
    return std::nullopt;
}

} // namespace returnguard::internal
