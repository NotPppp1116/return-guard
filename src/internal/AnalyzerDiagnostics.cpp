#include "Analyzer.hpp"

#include "DomainUtils.hpp"

#include <returnguard/Options.hpp>

#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/Basic/Diagnostic.h>
#include <llvm/ADT/StringRef.h>

#include <sstream>
#include <string>

namespace returnguard::internal {

std::string Analyzer::function_name(const clang::CallExpr* call) const {
    if (const clang::FunctionDecl* function = call->getDirectCallee()) {
        const std::string qualified = function->getQualifiedNameAsString();
        if (!qualified.empty()) {
            return qualified;
        }
    }

    std::string text = source_text(call->getCallee()->getSourceRange());
    return text.empty() ? "<indirect function>" : text;
}

void Analyzer::emit(
    const clang::CallExpr* call,
    llvm::StringRef message,
    llvm::StringRef note) const {
    clang::DiagnosticsEngine& diagnostics = context_.getDiagnostics();
    const clang::DiagnosticsEngine::Level level =
        returnguard::options().fail_on_diagnostics
            ? clang::DiagnosticsEngine::Error
            : clang::DiagnosticsEngine::Warning;

    clang::SourceLocation location = user_file_location(call->getExprLoc());
    if (location.isInvalid()) {
        location = call->getExprLoc();
    }

    const unsigned diagnostic_id = diagnostics.getCustomDiagID(
        level,
        "returnguard: %0");
    diagnostics.Report(location, diagnostic_id) << message;

    if (!note.empty()) {
        const unsigned note_id = diagnostics.getCustomDiagID(
            clang::DiagnosticsEngine::Note,
            "returnguard: %0");
        diagnostics.Report(location, note_id) << note;
    }
}

std::string Analyzer::missing_message(
    const std::vector<DomainValue>& missing) const {
    std::ostringstream out;
    out << "missing ";
    for (std::size_t index = 0; index < missing.size(); ++index) {
        if (index != 0U) {
            out << ", ";
        }
        out << value_display(missing[index]);
    }
    return out.str();
}

} // namespace returnguard::internal
