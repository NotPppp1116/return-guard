#include "Analyzer.hpp"

#include "AstUtils.hpp"
#include "DomainUtils.hpp"
#include "ReturnCollector.hpp"
#include "ValueSetInference.hpp"

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

void merge_values(Domain& destination, const Domain& source) {
    for (const DomainValue& value : source.values) {
        if (value.labels.empty()) {
            add_domain_value(destination, value.value, "");
            continue;
        }
        for (const std::string& label : value.labels) {
            add_domain_value(destination, value.value, label);
        }
    }
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

Domain Analyzer::infer_function_domain_once(const clang::FunctionDecl* function) {
    const clang::FunctionDecl* canonical = function->getCanonicalDecl();

    Domain annotated = annotation_domain(canonical);
    if (annotated.finite) {
        return annotated;
    }

    Domain by_type = type_domain(canonical->getReturnType());
    if (by_type.finite) {
        return by_type;
    }

    const clang::FunctionDecl* definition = nullptr;
    if (!canonical->hasBody(definition) || definition == nullptr) {
        return by_type;
    }

    std::vector<const clang::ReturnStmt*> return_statements;
    ReturnCollector collector(context_, return_statements);
    collector.TraverseStmt(const_cast<clang::Stmt*>(definition->getBody()));

    Domain inferred;
    inferred.type_name = canonical->getReturnType().getAsString();
    inferred.inferred_from_body = true;

    bool saw_value = false;
    bool complete = !return_statements.empty();
    std::unordered_set<const clang::FunctionDecl*> active_functions;
    active_functions.insert(canonical);

    for (const clang::ReturnStmt* statement : return_statements) {
        if (statement == nullptr || statement->getRetValue() == nullptr) {
            complete = false;
            continue;
        }

        std::unordered_set<const clang::VarDecl*> active_variables;
        const std::optional<Domain> part = expression_domain(
            statement->getRetValue(), active_functions, active_variables);
        if (!part.has_value() || !part->finite) {
            complete = false;
            continue;
        }

        saw_value = true;
        merge_values(inferred, *part);
    }

    inferred.finite = complete && saw_value && !inferred.values.empty();
    return inferred;
}

Domain Analyzer::function_domain(const clang::FunctionDecl* function,
                                 std::unordered_set<const clang::FunctionDecl*>& active) {
    const clang::FunctionDecl* canonical = function->getCanonicalDecl();

    if (summaries_building_ || summaries_prepared_) {
        if (const auto found = domain_cache_.find(canonical);
            found != domain_cache_.end()) {
            Domain result = found->second;
            if (collecting_domain_values_) {
                const Domain annotated = annotation_domain(canonical);
                const Domain by_type = type_domain(canonical->getReturnType());
                result.finite = annotated.finite || by_type.finite ||
                                !result.values.empty();
            } else if (validating_domain_completeness_) {
                const auto complete = domain_complete_.find(canonical);
                result.finite = complete != domain_complete_.end() &&
                                complete->second;
            }
            return result;
        }

        Domain annotated = annotation_domain(canonical);
        if (annotated.finite) {
            return annotated;
        }
        return type_domain(canonical->getReturnType());
    }

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

    std::vector<const clang::ReturnStmt*> return_statements;
    ReturnCollector collector(context_, return_statements);
    collector.TraverseStmt(const_cast<clang::Stmt*>(definition->getBody()));

    Domain inferred;
    inferred.finite = !return_statements.empty();
    inferred.inferred_from_body = true;
    inferred.type_name = function->getReturnType().getAsString();

    for (const clang::ReturnStmt* statement : return_statements) {
        if (statement == nullptr || statement->getRetValue() == nullptr) {
            inferred.finite = false;
            break;
        }

        std::unordered_set<const clang::VarDecl*> variables;
        const std::optional<Domain> part =
            expression_domain(statement->getRetValue(), active, variables);
        if (!part.has_value() || !part->finite) {
            inferred.finite = false;
            break;
        }
        merge_values(inferred, *part);
    }

    active.erase(canonical);
    if (!inferred.finite || inferred.values.empty()) {
        domain_cache_[canonical] = by_type;
        return by_type;
    }

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
