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

    for (const clang::AnnotateAttr* attribute :
         function->specific_attrs<clang::AnnotateAttr>()) {
        llvm::StringRef annotation = attribute->getAnnotation();
        constexpr llvm::StringLiteral prefix("returnguard.values:");
        if (!annotation.consume_front(prefix)) {
            continue;
        }

        domain.finite = true;
        domain.inferred_from_body = false;

        while (!annotation.empty()) {
            const auto split = annotation.split(',');
            llvm::StringRef token = split.first.trim();
            annotation = split.second;
            if (token.empty()) {
                continue;
            }

            const bool negative = token.consume_front("-");
            std::uint64_t magnitude = 0;
            if (token.getAsInteger(0, magnitude)) {
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
        return domain;
    }
    return domain;
}

std::optional<Domain> Analyzer::expression_domain(
    const clang::Expr* expression,
    std::unordered_set<const clang::FunctionDecl*>& active) {
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

    if (const auto* conditional =
            llvm::dyn_cast<clang::ConditionalOperator>(expression)) {
        auto true_domain = expression_domain(conditional->getTrueExpr(), active);
        auto false_domain = expression_domain(conditional->getFalseExpr(), active);
        if (!true_domain.has_value() || !false_domain.has_value() ||
            !true_domain->finite || !false_domain->finite) {
            return std::nullopt;
        }

        for (DomainValue& value : false_domain->values) {
            if (value.labels.empty()) {
                add_domain_value(*true_domain, value.value, "");
                continue;
            }
            for (const std::string& label : value.labels) {
                add_domain_value(*true_domain, value.value, label);
            }
        }
        return true_domain;
    }

    if (const auto* call = llvm::dyn_cast<clang::CallExpr>(expression)) {
        if (const clang::FunctionDecl* callee = call->getDirectCallee()) {
            return function_domain(callee, active);
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
        std::optional<Domain> part = expression_domain(return_expression, active);
        if (!part.has_value() || !part->finite) {
            inferred.finite = false;
            inferred.values.clear();
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

    if (!inferred.finite && by_type.finite) {
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
