#include "Instrumentation.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Attr.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Lex/Lexer.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <llvm/ADT/StringRef.h>

#include <optional>
#include <string>

namespace returnguard::internal {
namespace {

bool is_null_contract_name(llvm::StringRef name) {
    return name == "malloc" || name == "calloc" || name == "realloc" ||
           name == "aligned_alloc" || name == "fopen" || name == "freopen" ||
           name == "fdopen" || name == "tmpfile" || name == "opendir";
}

bool is_negative_contract_name(llvm::StringRef name) {
    return name == "open" || name == "open64" || name == "creat" ||
           name == "close" || name == "fsync" || name == "fdatasync" ||
           name == "ftruncate" || name == "truncate" || name == "dup" ||
           name == "dup2" || name == "dup3" || name == "socket" ||
           name == "accept" || name == "accept4" || name == "pipe" ||
           name == "pipe2" || name == "read" || name == "write" ||
           name == "pread" || name == "pwrite" || name == "recv" ||
           name == "send" || name == "printf" || name == "fprintf" ||
           name == "sprintf" || name == "snprintf" || name == "vprintf" ||
           name == "vfprintf" || name == "vsprintf" || name == "vsnprintf" ||
           name == "putchar" || name == "putc" || name == "puts" ||
           name == "fclose" || name == "fflush";
}

std::optional<llvm::StringRef>
contract_annotation(const clang::FunctionDecl& function) {
    for (const clang::FunctionDecl* redeclaration : function.redecls()) {
        for (const clang::AnnotateAttr* attribute :
             redeclaration->specific_attrs<clang::AnnotateAttr>()) {
            const llvm::StringRef annotation = attribute->getAnnotation();
            if (annotation == "returnguard.failure:null" ||
                annotation == "returnguard.failure:negative") {
                return annotation;
            }
        }
    }
    return std::nullopt;
}

} // namespace

Instrumentation::Instrumentation(clang::ASTContext& context,
                                 clang::Rewriter& rewriter)
    : context_(context), rewriter_(rewriter) {}

bool Instrumentation::should_instrument(const CheckResult& handling) const {
    return handling.kind == HandlingKind::Ignored ||
           handling.kind == HandlingKind::ExplicitlyIgnored ||
           handling.kind == HandlingKind::Consumed;
}

bool Instrumentation::consider(const clang::CallExpr* call,
                               const CheckResult& handling) {
    if (call == nullptr || !should_instrument(handling)) {
        return false;
    }

    const clang::FunctionDecl* callee = call->getDirectCallee();
    if (callee == nullptr || call->getCallReturnType(context_)->isVoidType()) {
        return false;
    }

    std::optional<FailurePredicate> predicate;
    if (const std::optional<llvm::StringRef> annotation =
            contract_annotation(*callee)) {
        predicate = *annotation == "returnguard.failure:null"
                        ? FailurePredicate::Null
                        : FailurePredicate::Negative;
    } else {
        const clang::SourceManager& source_manager = context_.getSourceManager();
        if (!source_manager.isInSystemHeader(callee->getLocation())) {
            return false;
        }

        const llvm::StringRef name = callee->getName();
        if (is_null_contract_name(name)) {
            predicate = FailurePredicate::Null;
        } else if (is_negative_contract_name(name)) {
            predicate = FailurePredicate::Negative;
        }
    }

    return predicate.has_value() && wrap_call(call, *predicate);
}

bool Instrumentation::wrap_call(const clang::CallExpr* call,
                                FailurePredicate predicate) {
    clang::SourceManager& source_manager = context_.getSourceManager();
    clang::SourceLocation begin = call->getBeginLoc();
    clang::SourceLocation end = call->getEndLoc();

    if (begin.isInvalid() || end.isInvalid() || begin.isMacroID() || end.isMacroID()) {
        return false;
    }

    begin = source_manager.getFileLoc(begin);
    end = source_manager.getFileLoc(end);
    if (!source_manager.isWrittenInMainFile(begin) ||
        !source_manager.isWrittenInMainFile(end) ||
        !rewriter_.isRewritable(begin) || !rewriter_.isRewritable(end)) {
        return false;
    }

    clang::CharSourceRange range = clang::CharSourceRange::getTokenRange(begin, end);
    range = clang::Lexer::makeFileCharRange(range, source_manager,
                                            context_.getLangOpts());
    if (range.isInvalid() || !ensure_runtime_header()) {
        return false;
    }

    const std::string prefix = predicate == FailurePredicate::Null
                                   ? "__RG_CHECK_NULL("
                                   : "__RG_CHECK_NEGATIVE(";
    const std::string suffix = ", " + std::to_string(site_id(call)) + "u)";

    if (rewriter_.InsertTextAfterToken(end, suffix)) {
        return false;
    }
    if (rewriter_.InsertTextBefore(begin, prefix)) {
        return false;
    }
    return true;
}

std::uint32_t Instrumentation::site_id(const clang::CallExpr* call) const {
    const clang::SourceManager& source_manager = context_.getSourceManager();
    const clang::SourceLocation location =
        source_manager.getFileLoc(call->getExprLoc());
    const std::string key = source_manager.getFilename(location).str() + ":" +
                            std::to_string(source_manager.getFileOffset(location));

    std::uint32_t hash = 2166136261u;
    for (const char character : key) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 16777619u;
    }
    return hash == 0u ? 1u : hash;
}

bool Instrumentation::ensure_runtime_header() {
    if (runtime_header_inserted_) {
        return true;
    }

    clang::SourceManager& source_manager = context_.getSourceManager();
    const clang::FileID main_file = source_manager.getMainFileID();
    const clang::SourceLocation start =
        source_manager.getLocForStartOfFile(main_file);
    if (start.isInvalid() || !rewriter_.isRewritable(start)) {
        return false;
    }

    if (rewriter_.InsertTextBefore(start,
                                   "#include <returnguard/Runtime.h>\n")) {
        return false;
    }
    runtime_header_inserted_ = true;
    return true;
}

} // namespace returnguard::internal
