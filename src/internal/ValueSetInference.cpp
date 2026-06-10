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
        add_domain_value(domain, *value, "");
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
        return std::nullopt;
    }

    if (!variable->isLocalVarDecl()) {
        return std::nullopt;
    }

    if (active_variables.contains(variable)) {
        return std::nullopt;
    }
    active_variables.insert(variable);

    const clang::DeclContext* dc = variable->getDeclContext();
    const auto* function = llvm::dyn_cast_or_null<clang::FunctionDecl>(dc);
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

    if (!finder.valid_ || finder.assignments_.empty()) {
        active_variables.erase(variable);
        return std::nullopt;
    }

    Domain combined;
    combined.finite = true;
    combined.inferred_from_body = true;
    combined.type_name = variable->getType().getAsString();

    for (const auto* assignment : finder.assignments_) {
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
