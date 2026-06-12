#include "Analyzer.hpp"

#include "AstUtils.hpp"
#include "ContractPolicy.hpp"
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

#include "ValueSetInference.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace returnguard::internal {
namespace {

llvm::APSInt signed_value(std::int64_t value) {
    llvm::APInt raw(64U, static_cast<std::uint64_t>(value), true);
    return llvm::APSInt(raw, false);
}

Domain negative_contract_domain(clang::QualType type) {
    Domain domain;
    domain.finite = true;
    domain.fallible_contract = true;
    domain.type_name = type.getAsString();
    add_domain_value(domain, signed_value(-1), "failure (<0)");
    add_domain_value(domain, signed_value(0), "success (>=0)");
    return domain;
}

Domain nonzero_contract_domain(clang::QualType type) {
    Domain domain;
    domain.finite = true;
    domain.fallible_contract = true;
    domain.type_name = type.getAsString();
    add_domain_value(domain, signed_value(1), "failure (!=0)");
    add_domain_value(domain, signed_value(0), "success (0)");
    return domain;
}

} // namespace

Domain Analyzer::enum_domain(const clang::EnumDecl* declaration) const {
    Domain domain;
    domain.finite = true;
    domain.type_name = declaration->getQualifiedNameAsString();

    for (const clang::EnumConstantDecl* enumerator : declaration->enumerators()) {
        add_domain_value(domain, enumerator->getInitVal(), enumerator->getQualifiedNameAsString());
    }
    return domain;
}

Domain Analyzer::type_domain(clang::QualType type) const {
    Domain domain;
    domain.type_name = type.getAsString();

    const clang::QualType canonical = type.getCanonicalType();
    if (canonical->isBooleanType()) {
        domain.finite = true;
        add_domain_value(domain, llvm::APSInt(llvm::APInt(1U, 0U), true), "false");
        add_domain_value(domain, llvm::APSInt(llvm::APInt(1U, 1U), true), "true");
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
                add_domain_value(domain, llvm::APSInt(raw, false),
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

std::optional<Domain>
Analyzer::expression_domain(const clang::Expr* expression,
                            std::unordered_set<const clang::FunctionDecl*>& active_functions,
                            std::unordered_set<const clang::VarDecl*>& active_variables) {
    ValueSetInference inference(*this);
    return inference.infer_expression(expression, active_functions, active_variables);
}

Domain Analyzer::function_domain(const clang::FunctionDecl* function,
                                 std::unordered_set<const clang::FunctionDecl*>& active) {
    const clang::FunctionDecl* canonical = function->getCanonicalDecl();
    if (const auto found = domain_cache_.find(canonical); found != domain_cache_.end()) {
        return found->second;
    }

    Domain by_type = type_domain(function->getReturnType());
    const std::optional<FailurePredicate> contract = failure_contract(*function, source_manager_);
    if (contract == FailurePredicate::Negative && function->getReturnType()->isSignedIntegerType()) {
        by_type = negative_contract_domain(function->getReturnType());
    } else if (contract == FailurePredicate::NonZero &&
               function->getReturnType()->isIntegerType()) {
        by_type = nonzero_contract_domain(function->getReturnType());
    }

    Domain annotated = annotation_domain(function);
    if (annotated.finite) {
        domain_cache_[canonical] = annotated;
        return annotated;
    }
    if (by_type.fallible_contract) {
        domain_cache_[canonical] = by_type;
        return by_type;
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

    std::vector<const clang::ReturnStmt*> return_stmts;
    ReturnCollector collector(context_, return_stmts);
    collector.TraverseStmt(const_cast<clang::Stmt*>(definition->getBody()));

    std::vector<const clang::Expr*> returns;
    for (const clang::ReturnStmt* stmt : return_stmts) {
        if (stmt != nullptr && stmt->getRetValue() != nullptr) {
            returns.push_back(stmt->getRetValue());
        }
    }

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
