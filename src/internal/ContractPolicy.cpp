#include "ContractPolicy.hpp"

#include <clang/AST/Attr.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/Basic/SourceManager.h>
#include <llvm/ADT/StringRef.h>

namespace returnguard::internal {
namespace {

const clang::DeclContext*
skip_linkage_contexts(const clang::DeclContext* context) {
    while (context != nullptr && context->isLinkageSpecContext()) {
        context = context->getParent();
    }
    return context;
}

bool is_global_context(const clang::FunctionDecl& function) {
    const clang::DeclContext* context =
        skip_linkage_contexts(function.getDeclContext());
    return context != nullptr && context->isTranslationUnit();
}

bool is_std_context(const clang::FunctionDecl& function) {
    const clang::DeclContext* context =
        skip_linkage_contexts(function.getDeclContext());

    while (context != nullptr) {
        const auto* namespace_declaration =
            llvm::dyn_cast<clang::NamespaceDecl>(context);
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

bool is_standard_null_name(llvm::StringRef name) {
    return name == "malloc" || name == "calloc" || name == "aligned_alloc" ||
           name == "fopen" || name == "freopen" || name == "tmpfile";
}

bool is_global_only_null_name(llvm::StringRef name) {
    return name == "fdopen" || name == "opendir";
}

bool is_standard_negative_name(llvm::StringRef name) {
    return name == "printf" || name == "fprintf" || name == "sprintf" ||
           name == "snprintf" || name == "vprintf" || name == "vfprintf" ||
           name == "vsprintf" || name == "vsnprintf" || name == "putchar" ||
           name == "putc" || name == "puts" || name == "fclose" ||
           name == "fflush";
}

bool is_global_only_negative_name(llvm::StringRef name) {
    /*
     * read/write/recv/send/accept remain excluded because EINTR, EAGAIN,
     * nonblocking descriptors, EOF, and partial transfers need policy-aware
     * handling rather than unconditional termination.
     */
    return name == "open" || name == "open64" || name == "creat" ||
           name == "close" || name == "fsync" || name == "fdatasync" ||
           name == "ftruncate" || name == "truncate" || name == "dup" ||
           name == "dup2" || name == "dup3" || name == "socket" ||
           name == "pipe" || name == "pipe2";
}

std::optional<FailurePredicate>
annotation_contract(const clang::FunctionDecl& function) {
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

std::optional<FailurePredicate>
failure_contract(const clang::FunctionDecl& function,
                 const clang::SourceManager& source_manager) {
    if (const std::optional<FailurePredicate> annotation =
            annotation_contract(function)) {
        return annotation;
    }

    if (!source_manager.isInSystemHeader(function.getLocation())) {
        return std::nullopt;
    }

    const llvm::StringRef name = function.getName();
    const bool global = is_global_context(function);
    const bool standard = is_std_context(function);

    if ((global || standard) && is_standard_null_name(name)) {
        return FailurePredicate::Null;
    }
    if (global && is_global_only_null_name(name)) {
        return FailurePredicate::Null;
    }
    if ((global || standard) && is_standard_negative_name(name)) {
        return FailurePredicate::Negative;
    }
    if (global && is_global_only_negative_name(name)) {
        return FailurePredicate::Negative;
    }
    return std::nullopt;
}

} // namespace returnguard::internal
