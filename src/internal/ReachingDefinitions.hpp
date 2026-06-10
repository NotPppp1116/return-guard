#pragma once

#include <vector>

namespace clang {
class ASTContext;
class Expr;
class FunctionDecl;
class VarDecl;
}

namespace returnguard::internal {

struct ReachingDefinitions {
    bool unknown = false;
    std::vector<const clang::Expr*> expressions;
};

ReachingDefinitions reaching_definitions_at(
    const clang::FunctionDecl& function,
    const clang::VarDecl& variable,
    const clang::Expr& reference,
    clang::ASTContext& context);

}
