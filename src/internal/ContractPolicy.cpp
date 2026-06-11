#include "ContractPolicy.hpp"

#include "AstUtils.hpp"

#include <clang/AST/Attr.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/Basic/SourceManager.h>
#include <llvm/ADT/StringRef.h>

namespace returnguard::internal {
namespace {

const clang::DeclContext* skip_linkage_contexts(const clang::DeclContext* context) {
    while (context != nullptr && context->getDeclKind() == clang::Decl::LinkageSpec) {
        context = context->getParent();
    }
    return context;
}

bool is_global_context(const clang::FunctionDecl& function) {
    const clang::DeclContext* context = skip_linkage_contexts(function.getDeclContext());
    return context != nullptr && context->isTranslationUnit();
}

bool is_std_context(const clang::FunctionDecl& function) {
    const clang::DeclContext* context = skip_linkage_contexts(function.getDeclContext());

    while (context != nullptr) {
        const auto* namespace_declaration = llvm::dyn_cast<clang::NamespaceDecl>(context);
        if (namespace_declaration == nullptr) {
            return false;
        }

        if (namespace_declaration->getName() == "std") {
            const clang::DeclContext* parent =
                skip_linkage_contexts(namespace_declaration->getParent());
            return parent != nullptr && parent->isTranslationUnit();
        }
        if (!namespace_declaration->isInline()) {
            return false;
        }
        context = skip_linkage_contexts(namespace_declaration->getParent());
    }
    return false;
}

std::optional<FailurePredicate> annotation_contract(const clang::FunctionDecl& function) {
    for (const clang::FunctionDecl* redeclaration : function.redecls()) {
        for (const clang::AnnotateAttr* attribute :
             redeclaration->specific_attrs<clang::AnnotateAttr>()) {
            const llvm::StringRef annotation = attribute->getAnnotation();
            if (annotation == "returnguard.failure:null") {
                return FailurePredicate::Null;
            }
            if (annotation == "returnguard.failure:negative") {
                return FailurePredicate::Negative;
            }
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<FailurePredicate> failure_contract(const clang::FunctionDecl& function,
                                                 const clang::SourceManager& source_manager) {
    if (const std::optional<FailurePredicate> annotation = annotation_contract(function)) {
        return annotation;
    }

    if (!source_manager.isInSystemHeader(function.getLocation())) {
        return std::nullopt;
    }

    const bool global = is_global_context(function);
    const bool standard = is_std_context(function);

    if ((global || standard) &&
        (has_identifier_name(&function, "fopen") || has_identifier_name(&function, "freopen") ||
         has_identifier_name(&function, "tmpfile"))) {
        return FailurePredicate::Null;
    }
    if (global &&
        (has_identifier_name(&function, "fdopen") || has_identifier_name(&function, "opendir"))) {
        return FailurePredicate::Null;
    }
    if ((global || standard) &&
        (has_identifier_name(&function, "printf") || has_identifier_name(&function, "fprintf") ||
         has_identifier_name(&function, "sprintf") || has_identifier_name(&function, "snprintf") ||
         has_identifier_name(&function, "vprintf") || has_identifier_name(&function, "vfprintf") ||
         has_identifier_name(&function, "vsprintf") ||
         has_identifier_name(&function, "vsnprintf") || has_identifier_name(&function, "putchar") ||
         has_identifier_name(&function, "putc") || has_identifier_name(&function, "puts") ||
         has_identifier_name(&function, "fclose") || has_identifier_name(&function, "fflush"))) {
        return FailurePredicate::Negative;
    }
    if (global &&
        (has_identifier_name(&function, "open") || has_identifier_name(&function, "open64") ||
         has_identifier_name(&function, "creat") || has_identifier_name(&function, "close") ||
         has_identifier_name(&function, "fsync") || has_identifier_name(&function, "fdatasync") ||
         has_identifier_name(&function, "ftruncate") ||
         has_identifier_name(&function, "truncate") || has_identifier_name(&function, "dup") ||
         has_identifier_name(&function, "dup2") || has_identifier_name(&function, "dup3") ||
         has_identifier_name(&function, "socket") || has_identifier_name(&function, "pipe") ||
         has_identifier_name(&function, "pipe2"))) {
        return FailurePredicate::Negative;
    }
    return std::nullopt;
}

} // namespace returnguard::internal
