#include "Instrumentation.hpp"

#include <returnguard/Options.hpp>

#include <clang/AST/ASTContext.h>
#include <clang/AST/ASTTypeTraits.h>
#include <clang/AST/Attr.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Lex/Lexer.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <llvm/ADT/StringRef.h>

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace returnguard::internal {
namespace {

bool is_null_contract_name(llvm::StringRef name) {
    /* realloc needs a dedicated contract because a zero size may legally return null. */
    return name == "malloc" || name == "calloc" || name == "aligned_alloc" ||
           name == "fopen" || name == "freopen" || name == "fdopen" ||
           name == "tmpfile" || name == "opendir";
}

bool is_negative_contract_name(llvm::StringRef name) {
    /*
     * Do not automatically instrument read/write/recv/send/accept. Their
     * failures are context-dependent: EINTR may require retry, EAGAIN is normal
     * for nonblocking descriptors, and successful transfers may be partial.
     * Projects can opt those functions in with RETURNGUARD_FAILS_NEGATIVE once
     * they have chosen an appropriate policy.
     */
    return name == "open" || name == "open64" || name == "creat" ||
           name == "close" || name == "fsync" || name == "fdatasync" ||
           name == "ftruncate" || name == "truncate" || name == "dup" ||
           name == "dup2" || name == "dup3" || name == "socket" ||
           name == "pipe" || name == "pipe2" || name == "printf" ||
           name == "fprintf" || name == "sprintf" || name == "snprintf" ||
           name == "vprintf" || name == "vfprintf" || name == "vsprintf" ||
           name == "vsnprintf" || name == "putchar" || name == "putc" ||
           name == "puts" || name == "fclose" || name == "fflush";
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

std::uint64_t hash_site_key(llvm::StringRef key) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char character : key) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ULL;
    }
    return hash == 0U ? 1U : hash;
}

bool path_stays_inside_root(const std::filesystem::path& relative) {
    if (relative.empty() || relative.is_absolute()) {
        return false;
    }
    const auto first = relative.begin();
    return first == relative.end() || *first != std::filesystem::path("..");
}

} // namespace

Instrumentation::Instrumentation(clang::ASTContext& context,
                                 clang::Rewriter& rewriter,
                                 std::vector<SiteMetadata>& sites)
    : context_(context), rewriter_(rewriter), sites_(sites) {}

bool Instrumentation::should_instrument(const CheckResult& handling) const {
    return handling.kind == HandlingKind::Ignored ||
           handling.kind == HandlingKind::ExplicitlyIgnored ||
           handling.kind == HandlingKind::Consumed;
}

bool Instrumentation::validate_contract(const clang::CallExpr* call,
                                        FailurePredicate predicate) const {
    const clang::QualType return_type = call->getCallReturnType(context_);
    const bool valid = predicate == FailurePredicate::Null
                           ? return_type->isPointerType()
                           : return_type->isSignedIntegerType();
    if (valid) {
        return true;
    }

    const clang::FunctionDecl* callee = call->getDirectCallee();
    const std::string function = callee == nullptr ? "<indirect>" : callee->getNameAsString();
    const char* requirement = predicate == FailurePredicate::Null
                                  ? "a pointer return type"
                                  : "a signed integer return type";
    clang::DiagnosticsEngine& diagnostics = context_.getDiagnostics();
    const unsigned diagnostic = diagnostics.getCustomDiagID(
        clang::DiagnosticsEngine::Error,
        "returnguard: failure contract on '%0' requires %1, but the function returns %2");
    diagnostics.Report(call->getExprLoc(), diagnostic)
        << function << requirement << return_type.getAsString();
    return false;
}

bool Instrumentation::consider(const clang::CallExpr* call,
                               const CheckResult& handling) {
    if (call == nullptr) {
        return false;
    }

    const clang::FunctionDecl* callee = call->getDirectCallee();
    if (callee == nullptr) {
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

    if (!predicate.has_value() || !validate_contract(call, *predicate) ||
        !should_instrument(handling)) {
        return false;
    }
    return wrap_call(call, *predicate);
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
    if (range.isInvalid()) {
        return false;
    }

    std::optional<SiteMetadata> metadata = metadata_for_call(call, predicate);
    if (!metadata.has_value() || !ensure_runtime_header()) {
        return false;
    }

    const std::string prefix = predicate == FailurePredicate::Null
                                   ? "__RG_CHECK_NULL("
                                   : "__RG_CHECK_NEGATIVE(";
    const std::string suffix = ", " + std::to_string(metadata->id) + "ULL)";

    if (rewriter_.InsertTextAfterToken(end, suffix)) {
        return false;
    }
    if (rewriter_.InsertTextBefore(begin, prefix)) {
        return false;
    }

    sites_.push_back(std::move(*metadata));
    return true;
}

std::optional<SiteMetadata>
Instrumentation::metadata_for_call(const clang::CallExpr* call,
                                   FailurePredicate predicate) {
    clang::SourceManager& source_manager = context_.getSourceManager();
    const clang::SourceLocation location =
        source_manager.getFileLoc(call->getExprLoc());
    if (location.isInvalid()) {
        return std::nullopt;
    }

    const clang::FunctionDecl* callee = call->getDirectCallee();
    SiteMetadata metadata;
    metadata.file = normalized_site_path(call);
    metadata.line = source_manager.getSpellingLineNumber(location);
    metadata.column = source_manager.getSpellingColumnNumber(location);
    metadata.function = enclosing_function_name(call);
    metadata.callee = callee == nullptr ? "<indirect>" : callee->getQualifiedNameAsString();
    metadata.predicate = predicate == FailurePredicate::Null ? "null" : "negative";

    const std::string key = metadata.file + '\x1f' +
                            std::to_string(metadata.line) + '\x1f' +
                            std::to_string(metadata.column) + '\x1f' +
                            metadata.function + '\x1f' + metadata.callee + '\x1f' +
                            metadata.predicate;
    metadata.id = hash_site_key(key);

    const auto [existing, inserted] = known_site_ids_.emplace(metadata.id, key);
    if (!inserted && existing->second != key) {
        clang::DiagnosticsEngine& diagnostics = context_.getDiagnostics();
        const unsigned diagnostic = diagnostics.getCustomDiagID(
            clang::DiagnosticsEngine::Error,
            "returnguard: site ID collision for %0; change the site-key scheme before building");
        diagnostics.Report(location, diagnostic)
            << static_cast<unsigned long long>(metadata.id);
        return std::nullopt;
    }

    return metadata;
}

std::string Instrumentation::normalized_site_path(const clang::CallExpr* call) const {
    const clang::SourceManager& source_manager = context_.getSourceManager();
    const clang::SourceLocation location =
        source_manager.getFileLoc(call->getExprLoc());
    std::filesystem::path file(source_manager.getFilename(location).str());
    if (file.empty()) {
        return "<unknown>";
    }

    std::error_code error;
    if (file.is_relative()) {
        const std::filesystem::path absolute = std::filesystem::absolute(file, error);
        if (!error) {
            file = absolute;
        }
    }
    file = file.lexically_normal();

    if (!returnguard::options().site_root.empty()) {
        std::filesystem::path root(returnguard::options().site_root);
        error.clear();
        if (root.is_relative()) {
            const std::filesystem::path absolute = std::filesystem::absolute(root, error);
            if (!error) {
                root = absolute;
            }
        }
        root = root.lexically_normal();

        const std::filesystem::path relative = file.lexically_relative(root);
        if (path_stays_inside_root(relative)) {
            file = relative;
        }
    }

    return file.generic_string();
}

std::string Instrumentation::enclosing_function_name(const clang::CallExpr* call) const {
    clang::DynTypedNode node = clang::DynTypedNode::create(*call);
    for (unsigned depth = 0U; depth < 128U; ++depth) {
        const auto parents = context_.getParents(node);
        if (parents.empty()) {
            break;
        }

        for (const clang::DynTypedNode& parent : parents) {
            if (const clang::FunctionDecl* function = parent.get<clang::FunctionDecl>()) {
                return function->getQualifiedNameAsString();
            }
        }
        node = parents[0];
    }
    return "<global>";
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
