#pragma once

#include "Model.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <llvm/ADT/APSInt.h>
#include <optional>
#include <unordered_set>
#include <vector>

namespace returnguard::internal {

class Analyzer;

class ValueSetInference {
  public:
    explicit ValueSetInference(Analyzer& analyzer);

    [[nodiscard]] std::optional<Domain>
    infer_expression(const clang::Expr* expression,
                     std::unordered_set<const clang::FunctionDecl*>& active_functions,
                     std::unordered_set<const clang::VarDecl*>& active_variables);

  private:
    [[nodiscard]] std::optional<Domain>
    infer_variable(const clang::VarDecl* variable, const clang::Expr* reference_site,
                   std::unordered_set<const clang::FunctionDecl*>& active_functions,
                   std::unordered_set<const clang::VarDecl*>& active_variables);

    Analyzer& analyzer_;
    const clang::ASTContext& context_;
};

} // namespace returnguard::internal
