#include <returnguard/Frontend.hpp>
#include <returnguard/Options.hpp>

#include "internal/Analyzer.hpp"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <llvm/ADT/StringRef.h>

#include <memory>

namespace returnguard {
namespace {

class Consumer final : public clang::ASTConsumer {
  public:
    explicit Consumer(clang::ASTContext& context) : analyzer_(context) {}

    void HandleTranslationUnit(clang::ASTContext& context) override {
        analyzer_.prepare_translation_unit();
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
        return std::make_unique<Consumer>(compiler.getASTContext());
    }
};

} // namespace

std::unique_ptr<clang::FrontendAction> make_frontend_action() {
    return std::make_unique<Action>();
}

std::unique_ptr<clang::FrontendAction> ActionFactory::create() {
    return make_frontend_action();
}

} // namespace returnguard
