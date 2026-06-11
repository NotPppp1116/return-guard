#include <returnguard/Frontend.hpp>
#include <returnguard/Options.hpp>

#include "internal/Analyzer.hpp"
#include "internal/SiteMetadata.hpp"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace returnguard {
namespace {

void write_json_string(llvm::raw_ostream& output, llvm::StringRef value) {
    static constexpr char hex[] = "0123456789abcdef";

    output << '"';
    for (const unsigned char character : value.bytes()) {
        switch (character) {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\b':
            output << "\\b";
            break;
        case '\f':
            output << "\\f";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (character < 0x20U) {
                output << "\\u00" << hex[character >> 4U]
                       << hex[character & 0x0fU];
            } else {
                output << static_cast<char>(character);
            }
            break;
        }
    }
    output << '"';
}

void report_write_error(clang::CompilerInstance& compiler,
                        llvm::StringRef kind,
                        llvm::StringRef message) {
    const unsigned diagnostic = compiler.getDiagnostics().getCustomDiagID(
        clang::DiagnosticsEngine::Error,
        "returnguard: cannot write %0: %1");
    compiler.getDiagnostics().Report(diagnostic) << kind << message;
}

bool write_site_map(clang::CompilerInstance& compiler,
                    llvm::ArrayRef<internal::SiteMetadata> sites) {
    if (options().site_map_output.empty()) {
        return true;
    }

    std::error_code error;
    llvm::raw_fd_ostream output(
        options().site_map_output,
        error,
        llvm::sys::fs::OF_Text);
    if (error) {
        report_write_error(compiler, "site map", error.message());
        return false;
    }

    std::vector<internal::SiteMetadata> sorted(sites.begin(), sites.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const internal::SiteMetadata& left,
                 const internal::SiteMetadata& right) {
                  return left.id < right.id;
              });

    output << "{\n  \"schema_version\": 1,\n  \"sites\": [";

    for (std::size_t index = 0U; index < sorted.size(); ++index) {
        const internal::SiteMetadata& site = sorted[index];
        output << (index == 0U ? "\n" : ",\n")
               << "    {\"id\": ";
        write_json_string(output, std::to_string(site.id));
        output << ", \"file\": ";
        write_json_string(output, site.file);
        output << ", \"line\": " << site.line
               << ", \"column\": " << site.column
               << ", \"function\": ";
        write_json_string(output, site.function);
        output << ", \"callee\": ";
        write_json_string(output, site.callee);
        output << ", \"callee_type\": ";
        write_json_string(output, site.callee_type);
        output << ", \"predicate\": ";
        write_json_string(output, site.predicate);
        output << '}';
    }

    if (!sorted.empty()) {
        output << '\n';
    }
    output << "  ]\n}\n";
    output.flush();
    if (output.has_error()) {
        report_write_error(compiler, "site map", "output stream failure");
        llvm::sys::fs::remove(options().site_map_output);
        return false;
    }
    return true;
}

class Consumer final : public clang::ASTConsumer {
  public:
    Consumer(clang::ASTContext& context,
             clang::Rewriter* rewriter,
             std::vector<internal::SiteMetadata>* sites)
        : analyzer_(context, rewriter, sites) {}

    void HandleTranslationUnit(clang::ASTContext& context) override {
        analyzer_.TraverseDecl(context.getTranslationUnitDecl());
    }

  private:
    internal::Analyzer analyzer_;
};

class Action final : public clang::ASTFrontendAction {
  public:
    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
        clang::CompilerInstance& compiler,
        llvm::StringRef) override {
        compiler.getDiagnosticOpts().ShowColors = options().color;

        clang::Rewriter* active_rewriter = nullptr;
        std::vector<internal::SiteMetadata>* active_sites = nullptr;
        if (!options().instrument_output.empty()) {
            rewriter_.setSourceMgr(compiler.getSourceManager(), compiler.getLangOpts());
            active_rewriter = &rewriter_;
            active_sites = &sites_;
        }
        return std::make_unique<Consumer>(
            compiler.getASTContext(), active_rewriter, active_sites);
    }

    void EndSourceFileAction() override {
        if (options().instrument_output.empty()) {
            return;
        }

        clang::CompilerInstance& compiler = getCompilerInstance();
        if (compiler.getDiagnostics().hasErrorOccurred()) {
            llvm::sys::fs::remove(options().instrument_output);
            if (!options().site_map_output.empty()) {
                llvm::sys::fs::remove(options().site_map_output);
            }
            return;
        }

        clang::SourceManager& source_manager = compiler.getSourceManager();
        const clang::FileID main_file = source_manager.getMainFileID();

        std::error_code error;
        llvm::raw_fd_ostream output(
            options().instrument_output,
            error,
            llvm::sys::fs::OF_Text);
        if (error) {
            report_write_error(compiler, "instrumented output", error.message());
            return;
        }

        if (const clang::RewriteBuffer* buffer =
                rewriter_.getRewriteBufferFor(main_file)) {
            output << std::string(buffer->begin(), buffer->end());
        } else {
            output << source_manager.getBufferData(main_file);
        }
        output.flush();
        if (output.has_error()) {
            report_write_error(compiler, "instrumented output", "output stream failure");
            llvm::sys::fs::remove(options().instrument_output);
            return;
        }

        if (!write_site_map(compiler, sites_)) {
            llvm::sys::fs::remove(options().instrument_output);
        }
    }

  private:
    clang::Rewriter rewriter_;
    std::vector<internal::SiteMetadata> sites_;
};

} // namespace

std::unique_ptr<clang::FrontendAction> make_frontend_action() {
    return std::make_unique<Action>();
}

std::unique_ptr<clang::FrontendAction> ActionFactory::create() {
    return make_frontend_action();
}

} // namespace returnguard
