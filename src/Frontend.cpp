#include <returnguard/Frontend.hpp>
#include <returnguard/Options.hpp>

#include "internal/Analyzer.hpp"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

#include <memory>
#include <string>
#include <system_error>

namespace returnguard {
namespace {

class Consumer final : public clang::ASTConsumer {
  public:
    Consumer(clang::ASTContext& context, clang::Rewriter* rewriter)
        : analyzer_(context, rewriter) {}

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
        if (!options().instrument_output.empty()) {
            rewriter_.setSourceMgr(compiler.getSourceManager(), compiler.getLangOpts());
            active_rewriter = &rewriter_;
        }
        return std::make_unique<Consumer>(compiler.getASTContext(), active_rewriter);
    }

    void EndSourceFileAction() override {
        if (options().instrument_output.empty()) {
            return;
        }

        clang::CompilerInstance& compiler = getCompilerInstance();
        clang::SourceManager& source_manager = compiler.getSourceManager();
        const clang::FileID main_file = source_manager.getMainFileID();

        std::error_code error;
        llvm::raw_fd_ostream output(
            options().instrument_output,
            error,
            llvm::sys::fs::OF_Text);
        if (error) {
            const unsigned diagnostic = compiler.getDiagnostics().getCustomDiagID(
                clang::DiagnosticsEngine::Error,
                "returnguard: cannot write instrumented output: %0");
            compiler.getDiagnostics().Report(diagnostic) << error.message();
            return;
        }

        if (const clang::RewriteBuffer* buffer =
                rewriter_.getRewriteBufferFor(main_file)) {
            output << std::string(buffer->begin(), buffer->end());
        } else {
            output << source_manager.getBufferData(main_file);
        }
    }

  private:
    clang::Rewriter rewriter_;
};

} // namespace

std::unique_ptr<clang::FrontendAction> make_frontend_action() {
    return std::make_unique<Action>();
}

std::unique_ptr<clang::FrontendAction> ActionFactory::create() {
    return make_frontend_action();
}

} // namespace returnguard
