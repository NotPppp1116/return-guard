#!/usr/bin/env python3
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one match, found {count}")
    return text.replace(old, new, 1)


source_path = Path("src/internal/AnalyzerSafety.cpp")
source = source_path.read_text(encoding="utf-8")

source = replace_once(
    source,
    "#include <clang/AST/ASTContext.h>\n",
    "#include <clang/AST/ASTContext.h>\n#include <clang/AST/ParentMapContext.h>\n",
    "ParentMapContext include",
)

helper = r'''    const clang::CXXForRangeStmt* enclosing_range_for(const clang::VarDecl* variable) {
        if (variable == nullptr || !variable->isCXXForRangeDecl())
            return nullptr;

        const auto parents = context_.getParents(*variable);
        for (const clang::DynTypedNode& parent : parents) {
            if (const auto* range = parent.get<clang::CXXForRangeStmt>())
                return range;
            if (const auto* declarationStatement = parent.get<clang::DeclStmt>()) {
                const auto grandparents = context_.getParents(*declarationStatement);
                for (const clang::DynTypedNode& grandparent : grandparents) {
                    if (const auto* range = grandparent.get<clang::CXXForRangeStmt>())
                        return range;
                }
            }
        }
        return nullptr;
    }

    bool expression_has_current_frame_storage(const clang::Expr* expression) {
        expression = strip_expr(expression);
        if (expression == nullptr)
            return false;

        if (llvm::isa<clang::CXXThisExpr>(expression))
            return false;

        if (const auto* reference = llvm::dyn_cast<clang::DeclRefExpr>(expression)) {
            const auto* variable = llvm::dyn_cast<clang::VarDecl>(reference->getDecl());
            if (variable == nullptr || llvm::isa<clang::ParmVarDecl>(variable) || variable->isStaticLocal())
                return false;
            if (variable->getType()->isReferenceType())
                return reference_denotes_current_frame_storage(variable);
            return variable->isLocalVarDecl();
        }

        if (const auto* member = llvm::dyn_cast<clang::MemberExpr>(expression)) {
            // The automatic storage of a local pointer says nothing about its pointee.
            return !member->isArrow() && expression_has_current_frame_storage(member->getBase());
        }

        if (const auto* conditional = llvm::dyn_cast<clang::ConditionalOperator>(expression)) {
            return expression_has_current_frame_storage(conditional->getTrueExpr()) ||
                   expression_has_current_frame_storage(conditional->getFalseExpr());
        }

        if (const auto* conditional = llvm::dyn_cast<clang::BinaryConditionalOperator>(expression)) {
            return expression_has_current_frame_storage(conditional->getCommon()) ||
                   expression_has_current_frame_storage(conditional->getFalseExpr());
        }

        if (const auto* cleanup = llvm::dyn_cast<clang::ExprWithCleanups>(expression))
            return expression_has_current_frame_storage(cleanup->getSubExpr());
        if (const auto* temporary = llvm::dyn_cast<clang::MaterializeTemporaryExpr>(expression))
            return expression_has_current_frame_storage(temporary->getSubExpr());
        if (const auto* temporary = llvm::dyn_cast<clang::CXXBindTemporaryExpr>(expression))
            return expression_has_current_frame_storage(temporary->getSubExpr());

        if (const auto* call = llvm::dyn_cast<clang::CallExpr>(expression))
            return !call->getType()->isReferenceType();

        return llvm::isa<clang::CXXConstructExpr, clang::InitListExpr>(expression);
    }

    bool reference_denotes_current_frame_storage(const clang::VarDecl* variable) {
        if (variable == nullptr || !variable->getType()->isReferenceType())
            return false;
        if (const clang::CXXForRangeStmt* range = enclosing_range_for(variable))
            return expression_has_current_frame_storage(range->getRangeInit());
        return variable->getInit() != nullptr && expression_has_current_frame_storage(variable->getInit());
    }

'''

source = replace_once(
    source,
    "  private:\n    bool is_stack_address(const clang::Expr* expression) {\n",
    "  private:\n" + helper + "    bool is_stack_address(const clang::Expr* expression) {\n",
    "stack-storage helpers insertion",
)

old_address_check = r'''            if (unary->getOpcode() == clang::UO_AddrOf) {
                const clang::Expr* sub = unary->getSubExpr()->IgnoreParenCasts();
                if (const auto* ref = llvm::dyn_cast<clang::DeclRefExpr>(sub)) {
                    if (const auto* var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl())) {
                        return var->isLocalVarDecl() && !var->isStaticLocal();
                    }
                }
            }
'''
new_address_check = r'''            if (unary->getOpcode() == clang::UO_AddrOf)
                return expression_has_current_frame_storage(unary->getSubExpr());
'''
source = replace_once(
    source,
    old_address_check,
    new_address_check,
    "address-of stack classification",
)
source_path.write_text(source, encoding="utf-8")

case_path = Path("tests/cases/safety.cpp")
cases = case_path.read_text(encoding="utf-8")
regression_cases = r'''

struct RangeItem {
    int value;
};

struct RangeOwner {
    RangeItem items[2];

    RangeItem* find_member_item() {
        for (auto& item : items) {
            if (item.value == 1)
                return &item;
        }
        return nullptr;
    }
};

RangeItem* find_parameter_item(RangeItem (&items)[2]) {
    for (auto& item : items) {
        if (item.value == 1)
            return &item;
    }
    return nullptr;
}
'''
if "RangeItem* find_parameter_item" in cases:
    raise RuntimeError("range-for regression cases already exist")
case_path.write_text(cases.rstrip() + regression_cases + "\n", encoding="utf-8")
