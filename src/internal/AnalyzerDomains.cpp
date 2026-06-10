#include "Analyzer.hpp"

#include "AstUtils.hpp"
#include "DomainUtils.hpp"
#include "ReturnCollector.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Attr.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/Type.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Casting.h>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace returnguard::internal {
namespace {

class AssignmentCollector final : public clang::RecursiveASTVisitor<AssignmentCollector> {
  public:
    explicit AssignmentCollector(const clang::VarDecl* target) : target_(target) {}

    bool VisitBinaryOperator(clang::BinaryOperator* op) {
        if (op->isAssignmentOp()) {
            if (referenced_variable(op->getLHS()) == target_) {
                if (op->getOpcode() == clang::BO_Assign) {
                    assignments_.push_back(op->getRHS());
                } else {
                    valid_ = false;
                    return false;
                }
            }
        }
        return true;
    }

    bool VisitUnaryOperator(clang::UnaryOperator* op) {
        if (op->isIncrementDecrementOp()) {
            if (referenced_variable(op->getSubExpr()) == target_) {
                valid_ = false;
                return false;
            }
        } else if (op->getOpcode() == clang::UO_AddrOf) {
            if (referenced_variable(op->getSubExpr()) == target_) {
                valid_ = false;
                return false;
            }
        }
        return true;
    }

    bool VisitCallExpr(clang::CallExpr* call) {
        for (const clang::Expr* argument : call->arguments()) {
            if (expression_references_variable(argument, target_)) {
                valid_ = false;
                return false;
            }
        }
        return true;
    }

    bool VisitIfStmt(clang::IfStmt* if_stmt) {
        // Condition might have assignments if it's an assignment expression
        return true;
    }

    bool VisitSwitchStmt(clang::SwitchStmt* sw) {
        if (!expression_references_variable(sw->getCond(), target_)) {
            // If the switch condition doesn't involve the target, 
            // the body might still have assignments.
            return true; 
        }
        return true;
    }

    bool VisitStmt(clang::Stmt* s) {
        if (const auto* ds = llvm::dyn_cast<clang::DeclStmt>(s)) {
            for (const auto* d : ds->decls()) {
                if (d == target_) {
                    // Initialization is handled separately
                }
            }
        }
        return true;
    }

    bool valid_ = true;
    std::vector<const clang::Expr*> assignments_;
    const clang::VarDecl* target_;
};

} // namespace

Domain Analyzer::enum_domain(const clang::EnumDecl* declaration) const {
    Domain domain;
    domain.finite = true;
    domain.type_name = declaration->getQualifiedNameAsString();

    for (const clang::EnumConstantDecl* enumerator : declaration->enumerators()) {
        add_domain_value(
            domain,
            enumerator->getInitVal(),
            enumerator->getQualifiedNameAsString());
    }
    return domain;
}

Domain Analyzer::type_domain(clang::QualType type) const {
    Domain domain;
    domain.type_name = type.getAsString();

    const clang::QualType canonical = type.getCanonicalType();
    if (canonical->isBooleanType()) {
        domain.finite = true;
        add_domain_value(
            domain,
            llvm::APSInt(llvm::APInt(1U, 0U), true),
            "false");
        add_domain_value(
            domain,
            llvm::APSInt(llvm::APInt(1U, 1U), true),
            "true");
        return domain;
    }

    if (const auto* enum_type = canonical->getAs<clang::EnumType>()) {
        return enum_domain(enum_type->getDecl());
    }

    return domain;
}

Domain Analyzer::annotation_domain(const clang::FunctionDecl* function) const {
    Domain domain;
    domain.type_name = function->getReturnType().getAsString();

    bool found_annotation = false;
    for (const clang::FunctionDecl* redeclaration : function->redecls()) {
        for (const clang::AnnotateAttr* attribute :
             redeclaration->specific_attrs<clang::AnnotateAttr>()) {
            llvm::StringRef annotation = attribute->getAnnotation();
            constexpr llvm::StringLiteral prefix("returnguard.values:");
            if (!annotation.consume_front(prefix)) {
                continue;
            }

            found_annotation = true;
            bool found_value = false;

            while (!annotation.empty()) {
                const auto split = annotation.split(',');
                llvm::StringRef token = split.first.trim();
                annotation = split.second;
                if (token.empty()) {
                    continue;
                }

                found_value = true;
                const bool negative = token.consume_front("-");
                (void)token.consume_front("+");

                std::uint64_t magnitude = 0;
                if (token.empty() || token.getAsInteger(0, magnitude)) {
                    domain.finite = false;
                    domain.values.clear();
                    return domain;
                }

                llvm::APInt raw(64U, magnitude);
                if (negative) {
                    raw = -raw;
                }
                add_domain_value(
                    domain,
                    llvm::APSInt(raw, false),
                    negative ? "-" + std::to_string(magnitude)
                             : std::to_string(magnitude));
            }

            if (!found_value) {
                domain.finite = false;
                domain.values.clear();
                return domain;
            }
        }
    }

    domain.finite = found_annotation && !domain.values.empty();
    domain.inferred_from_body = false;
    return domain;
}

std::optional<Domain> Analyzer::expression_domain(
    const clang::Expr* expression,
    std::unordered_set<const clang::FunctionDecl*>& active_functions,
    std::unordered_set<const clang::VarDecl*>& active_variables) {
    expression = strip_expr(expression);
    if (expression == nullptr) {
        return std::nullopt;
    }

    if (const std::optional<llvm::APSInt> value =
            evaluate_integer(expression, context_)) {
        Domain domain;
        domain.finite = true;
        domain.inferred_from_body = true;
        domain.type_name = expression->getType().getAsString();
        add_domain_value(domain, *value, source_text(expression->getSourceRange()));
        return domain;
    }

    if (const auto* reference = llvm::dyn_cast<clang::DeclRefExpr>(expression)) {
        if (const auto* variable = llvm::dyn_cast<clang::VarDecl>(reference->getDecl())) {
            if (llvm::isa<clang::ParmVarDecl>(variable)) {
                return std::nullopt;
            }

            if (variable->isLocalVarDecl()) {
                if (active_variables.contains(variable)) {
                    return std::nullopt;
                }
                active_variables.insert(variable);

                const clang::FunctionDecl* enclosing = enclosing_function(reference);
                const clang::FunctionDecl* definition = nullptr;
                if (enclosing && enclosing->hasBody(definition)) {
                    AssignmentCollector collector(variable);
                    if (variable->hasInit()) {
                        collector.assignments_.push_back(variable->getInit());
                    }
                    collector.TraverseStmt(const_cast<clang::Stmt*>(definition->getBody()));

                    if (collector.valid_ && !collector.assignments_.empty()) {
                        Domain combined;
                        combined.finite = true;
                        combined.inferred_from_body = true;
                        combined.type_name = variable->getType().getAsString();

                        std::vector<const clang::Expr*> reaching_assignments;
                        // Simple reaching definition analysis:
                        // For a local variable, we look at all assignments.
                        // We filter out those that are strictly before a later unconditional assignment.
                        // For now, let's just use all found assignments as it is safer.
                        reaching_assignments = collector.assignments_;

                        for (const clang::Expr* assignment : reaching_assignments) {
                            auto part = expression_domain(
                                assignment, active_functions, active_variables);
                            if (!part || !part->finite) {
                                combined.finite = false;
                                break;
                            }
                            for (const DomainValue& value : part->values) {
                                if (value.labels.empty()) {
                                    add_domain_value(combined, value.value, "");
                                } else {
                                    for (const std::string& label : value.labels) {
                                        add_domain_value(combined, value.value, label);
                                    }
                                }
                            }
                        }

                        active_variables.erase(variable);
                        if (combined.finite) {
                            return combined;
                        }
                        return std::nullopt;
                    }
                }
                active_variables.erase(variable);
            }
        }
    }

    if (const auto* conditional =
            llvm::dyn_cast<clang::ConditionalOperator>(expression)) {
        auto true_domain =
            expression_domain(conditional->getTrueExpr(), active_functions, active_variables);
        auto false_domain =
            expression_domain(conditional->getFalseExpr(), active_functions, active_variables);
        if (!true_domain.has_value() || !false_domain.has_value() || !true_domain->finite ||
            !false_domain->finite) {
            return std::nullopt;
        }

        Domain combined = *true_domain;
        for (DomainValue& value : false_domain->values) {
            if (value.labels.empty()) {
                add_domain_value(combined, value.value, "");
                continue;
            }
            for (const std::string& label : value.labels) {
                add_domain_value(combined, value.value, label);
            }
        }
        combined.inferred_from_body = true;
        return combined;
    }

    if (const auto* call = llvm::dyn_cast<clang::CallExpr>(expression)) {
        if (const clang::FunctionDecl* callee = call->getDirectCallee()) {
            return function_domain(callee, active_functions);
        }
    }

    return std::nullopt;
}

Domain Analyzer::function_domain(
    const clang::FunctionDecl* function,
    std::unordered_set<const clang::FunctionDecl*>& active) {
    const clang::FunctionDecl* canonical = function->getCanonicalDecl();
    if (const auto found = domain_cache_.find(canonical);
        found != domain_cache_.end()) {
        return found->second;
    }

    Domain by_type = type_domain(function->getReturnType());
    Domain annotated = annotation_domain(function);
    if (annotated.finite) {
        domain_cache_[canonical] = annotated;
        return annotated;
    }

    if (active.contains(canonical)) {
        return by_type;
    }
    active.insert(canonical);

    const clang::FunctionDecl* definition = nullptr;
    if (!function->hasBody(definition) || definition == nullptr) {
        active.erase(canonical);
        domain_cache_[canonical] = by_type;
        return by_type;
    }

    std::vector<const clang::Expr*> returns;
    ReturnCollector collector(context_, returns);
    collector.TraverseStmt(const_cast<clang::Stmt*>(definition->getBody()));

    if (returns.empty()) {
        active.erase(canonical);
        domain_cache_[canonical] = by_type;
        return by_type;
    }

    Domain inferred;
    inferred.finite = true;
    inferred.inferred_from_body = true;
    inferred.type_name = function->getReturnType().getAsString();

    for (const clang::Expr* return_expression : returns) {
        std::unordered_set<const clang::VarDecl*> variables;
        std::optional<Domain> part = expression_domain(return_expression, active, variables);
        if (!part.has_value() || !part->finite) {
            inferred.finite = false;
            break;
        }

        for (DomainValue& value : part->values) {
            if (value.labels.empty()) {
                add_domain_value(inferred, value.value, "");
            } else {
                for (const std::string& label : value.labels) {
                    add_domain_value(inferred, value.value, label);
                }
            }
        }
    }

    active.erase(canonical);

    if (!inferred.finite) {
        active.erase(canonical);
        domain_cache_[canonical] = by_type;
        return by_type;
    }

    inferred.inferred_from_body = true;
    domain_cache_[canonical] = inferred;
    return inferred;
}

Domain Analyzer::function_domain(const clang::FunctionDecl* function) {
    std::unordered_set<const clang::FunctionDecl*> active;
    return function_domain(function, active);
}

Domain Analyzer::call_domain(const clang::CallExpr* call) {
    if (const clang::FunctionDecl* function = call->getDirectCallee()) {
        return function_domain(function);
    }
    return type_domain(call->getCallReturnType(context_));
}

} // namespace returnguard::internal
