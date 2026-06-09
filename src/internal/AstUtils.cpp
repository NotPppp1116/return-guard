#include "AstUtils.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Attr.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <llvm/Support/Casting.h>

namespace returnguard::internal {
namespace {

bool function_is_no_return(const clang::FunctionDecl* function) {
    return function != nullptr &&
           (function->isNoReturn() || function->hasAttr<clang::AnalyzerNoReturnAttr>() ||
            function->hasAttr<clang::C11NoReturnAttr>() ||
            function->hasAttr<clang::CXX11NoReturnAttr>() ||
            function->hasAttr<clang::InferredNoReturnAttr>() ||
            function->hasAttr<clang::NoReturnAttr>());
}

} // namespace

const clang::Expr* strip_expr(const clang::Expr* expression) {
    return expression == nullptr ? nullptr : expression->IgnoreParenImpCasts();
}

std::optional<llvm::APSInt> evaluate_integer(const clang::Expr* expression,
                                             const clang::ASTContext& context) {
    if (expression == nullptr || expression->isValueDependent()) {
        return std::nullopt;
    }
    return expression->getIntegerConstantExpr(context);
}

const clang::VarDecl* referenced_variable(const clang::Expr* expression) {
    expression = strip_expr(expression);
    const auto* reference = llvm::dyn_cast_or_null<clang::DeclRefExpr>(expression);
    if (reference == nullptr) {
        return nullptr;
    }
    return llvm::dyn_cast<clang::VarDecl>(reference->getDecl());
}

bool expression_forwards_variable(const clang::Expr* expression, const clang::VarDecl* variable) {
    expression = strip_expr(expression);
    if (expression == nullptr || variable == nullptr) {
        return false;
    }

    if (referenced_variable(expression) == variable) {
        return true;
    }

    if (const auto* cast = llvm::dyn_cast<clang::CStyleCastExpr>(expression)) {
        return expression_forwards_variable(cast->getSubExpr(), variable);
    }
    if (const auto* cast = llvm::dyn_cast<clang::CXXStaticCastExpr>(expression)) {
        return expression_forwards_variable(cast->getSubExpr(), variable);
    }
    if (const auto* cast = llvm::dyn_cast<clang::CXXFunctionalCastExpr>(expression)) {
        return expression_forwards_variable(cast->getSubExpr(), variable);
    }
    if (const auto* cleanup = llvm::dyn_cast<clang::ExprWithCleanups>(expression)) {
        return expression_forwards_variable(cleanup->getSubExpr(), variable);
    }
    if (const auto* temporary = llvm::dyn_cast<clang::MaterializeTemporaryExpr>(expression)) {
        return expression_forwards_variable(temporary->getSubExpr(), variable);
    }
    if (const auto* temporary = llvm::dyn_cast<clang::CXXBindTemporaryExpr>(expression)) {
        return expression_forwards_variable(temporary->getSubExpr(), variable);
    }
    if (const auto* binary = llvm::dyn_cast<clang::BinaryOperator>(expression);
        binary != nullptr && binary->getOpcode() == clang::BO_Comma) {
        return expression_forwards_variable(binary->getRHS(), variable);
    }

    return false;
}

bool statement_exits(const clang::Stmt* statement) {
    if (statement == nullptr) {
        return false;
    }

    statement = statement->IgnoreContainers();
    if (llvm::isa<clang::ReturnStmt, clang::BreakStmt, clang::ContinueStmt, clang::GotoStmt>(
            statement)) {
        return true;
    }

    if (const auto* compound = llvm::dyn_cast<clang::CompoundStmt>(statement)) {
        for (const clang::Stmt* child : compound->body()) {
            if (statement_exits(child)) {
                return true;
            }
        }
        return false;
    }

    if (const auto* if_statement = llvm::dyn_cast<clang::IfStmt>(statement)) {
        return statement_exits(if_statement->getThen()) && statement_exits(if_statement->getElse());
    }

    const auto* expression = llvm::dyn_cast<clang::Expr>(statement);
    const auto* call = llvm::dyn_cast_or_null<clang::CallExpr>(strip_expr(expression));
    if (call != nullptr) {
        return function_is_no_return(call->getDirectCallee());
    }

    return false;
}

bool expression_references_variable(const clang::Stmt* statement, const clang::VarDecl* variable) {
    if (statement == nullptr || variable == nullptr) {
        return false;
    }

    class Finder final : public clang::RecursiveASTVisitor<Finder> {
      public:
        explicit Finder(const clang::VarDecl* target) : target_(target) {}

        bool VisitDeclRefExpr(clang::DeclRefExpr* reference) {
            if (reference->getDecl() == target_) {
                found_ = true;
                return false;
            }
            return true;
        }

        [[nodiscard]] bool found() const { return found_; }

      private:
        const clang::VarDecl* target_;
        bool found_ = false;
    } finder(variable);

    finder.TraverseStmt(const_cast<clang::Stmt*>(statement));
    return finder.found();
}

} // namespace returnguard::internal
