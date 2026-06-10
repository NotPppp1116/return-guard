#include "Analyzer.hpp"

#include <returnguard/Options.hpp>

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/ParentMapContext.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Lex/Lexer.h>

namespace returnguard::internal {

clang::SourceLocation Analyzer::user_file_location(clang::SourceLocation location) const {
    if (location.isInvalid()) {
        return location;
    }

    // Clang's AST is produced after macro expansion. getFileLoc() maps a token
    // from a macro argument to the argument spelling and a token from a macro
    // body to the invocation site. Diagnostics therefore point into user code.
    return source_manager_.getFileLoc(location);
}

bool Analyzer::should_analyze_location(clang::SourceLocation location) const {
    location = user_file_location(location);
    if (location.isInvalid() || source_manager_.isInSystemHeader(location)) {
        return false;
    }
    if (returnguard::options().analyze_headers) {
        return true;
    }
    return source_manager_.isWrittenInMainFile(location);
}

std::string Analyzer::source_text(clang::SourceRange range) const {
    clang::CharSourceRange char_range = clang::CharSourceRange::getTokenRange(range);

    char_range =
        clang::Lexer::makeFileCharRange(char_range, source_manager_, context_.getLangOpts());
    if (char_range.isInvalid()) {
        return {};
    }

    return clang::Lexer::getSourceText(char_range, source_manager_, context_.getLangOpts()).str();
}

const clang::FunctionDecl* Analyzer::enclosing_function(const clang::Stmt* statement) const {
    clang::DynTypedNode node = clang::DynTypedNode::create(*statement);
    for (unsigned depth = 0; depth < 128U; ++depth) {
        const auto parents = context_.getParents(node);
        if (parents.empty()) {
            return nullptr;
        }
        const clang::DynTypedNode& parent = parents[0];
        if (const auto* function = parent.get<clang::FunctionDecl>()) {
            return function;
        }
        node = parent;
    }
    return nullptr;
}

bool Analyzer::is_explicit_void_discard(const clang::CallExpr* call) const {
    clang::DynTypedNode current = clang::DynTypedNode::create(*call);
    for (unsigned depth = 0; depth < 16U; ++depth) {
        const auto parents = context_.getParents(current);
        if (parents.empty()) {
            return false;
        }
        const clang::DynTypedNode& parent = parents[0];

        if (const auto* cast = parent.get<clang::CStyleCastExpr>()) {
            return cast->getType()->isVoidType();
        }
        if (const auto* cast = parent.get<clang::CXXStaticCastExpr>()) {
            return cast->getType()->isVoidType();
        }
        if (const auto* cast = parent.get<clang::CXXFunctionalCastExpr>()) {
            return cast->getType()->isVoidType();
        }

        if (parent.get<clang::ParenExpr>() != nullptr ||
            parent.get<clang::ImplicitCastExpr>() != nullptr ||
            parent.get<clang::ExprWithCleanups>() != nullptr ||
            parent.get<clang::MaterializeTemporaryExpr>() != nullptr ||
            parent.get<clang::CXXBindTemporaryExpr>() != nullptr) {
            current = parent;
            continue;
        }
        return false;
    }
    return false;
}

} // namespace returnguard::internal
