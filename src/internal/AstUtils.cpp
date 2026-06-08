#include "AstUtils.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <llvm/Support/Casting.h>

namespace returnguard::internal {

const clang::Expr* strip_expr(const clang::Expr* expression) {
    return expression == nullptr ? nullptr : expression->IgnoreParenImpCasts();
}

std::optional<llvm::APSInt> evaluate_integer(
    const clang::Expr* expression,
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

bool expression_references_variable(
    const clang::Stmt* statement,
    const clang::VarDecl* variable) {
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
