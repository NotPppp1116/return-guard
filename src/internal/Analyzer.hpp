#pragma once

#include "Model.hpp"

#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Type.h>
#include <clang/Basic/SourceLocation.h>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace clang {
class ASTContext;
class CallExpr;
class DoStmt;
class EnumDecl;
class Expr;
class ForStmt;
class FunctionDecl;
class IfStmt;
class SourceManager;
class Stmt;
class SwitchStmt;
class VarDecl;
class WhileStmt;
} // namespace clang

namespace llvm {
class StringRef;
}

namespace returnguard::internal {

class CFGValueFlow;
class HandlerFinder;
class NullStateAnalysis;

class Analyzer final : public clang::RecursiveASTVisitor<Analyzer> {
  public:
    explicit Analyzer(clang::ASTContext& context);
    ~Analyzer();

    [[nodiscard]] bool shouldVisitTemplateInstantiations() const;
    [[nodiscard]] bool shouldVisitImplicitCode() const;
    bool VisitCallExpr(clang::CallExpr* call);

    [[nodiscard]] const clang::ASTContext& context() const;
    [[nodiscard]] const clang::SourceManager& source_manager() const;

    [[nodiscard]] CheckResult analyze_switch(const clang::SwitchStmt* statement,
                                             const Domain& domain) const;
    [[nodiscard]] CheckResult analyze_if_chain(const clang::IfStmt* statement,
                                               const clang::VarDecl* variable,
                                               const Domain& domain) const;
    [[nodiscard]] CheckResult analyze_if_chain(const clang::IfStmt* statement,
                                               const ExpressionSet& aliases,
                                               const Domain& domain) const;
    [[nodiscard]] CheckResult analyze_condition(const clang::Expr* condition,
                                                const clang::VarDecl* variable,
                                                const Domain& domain) const;
    [[nodiscard]] CheckResult analyze_condition(const clang::Expr* condition,
                                                const ExpressionSet& aliases,
                                                const Domain& domain) const;

  private:
    friend class HandlerFinder;

    [[nodiscard]] clang::SourceLocation user_file_location(clang::SourceLocation location) const;
    [[nodiscard]] bool should_analyze_location(clang::SourceLocation location) const;
    [[nodiscard]] std::string source_text(clang::SourceRange range) const;
    [[nodiscard]] const clang::FunctionDecl* enclosing_function(const clang::Stmt* statement) const;
    [[nodiscard]] bool is_explicit_void_discard(const clang::CallExpr* call) const;

    [[nodiscard]] Domain enum_domain(const clang::EnumDecl* declaration) const;
    [[nodiscard]] Domain type_domain(clang::QualType type) const;
    [[nodiscard]] Domain annotation_domain(const clang::FunctionDecl* function) const;
    [[nodiscard]] std::optional<Domain>
    expression_domain(const clang::Expr* expression,
                      std::unordered_set<const clang::FunctionDecl*>& active);
    [[nodiscard]] Domain function_domain(const clang::FunctionDecl* function,
                                         std::unordered_set<const clang::FunctionDecl*>& active);
    [[nodiscard]] Domain function_domain(const clang::FunctionDecl* function);
    [[nodiscard]] Domain call_domain(const clang::CallExpr* call);

    [[nodiscard]] const clang::VarDecl*
    variable_initialized_by_call(const clang::CallExpr* call) const;
    [[nodiscard]] const clang::VarDecl*
    variable_assigned_from_call(const clang::CallExpr* call) const;
    [[nodiscard]] const clang::SwitchStmt*
    enclosing_direct_switch(const clang::CallExpr* call) const;
    [[nodiscard]] const clang::IfStmt* enclosing_direct_if(const clang::CallExpr* call) const;
    [[nodiscard]] const clang::Expr*
    enclosing_direct_loop_condition(const clang::CallExpr* call) const;
    [[nodiscard]] const clang::Expr*
    enclosing_direct_conditional_condition(const clang::CallExpr* call) const;
    [[nodiscard]] const clang::Expr*
    enclosing_assignment_condition(const clang::CallExpr* call,
                                   const clang::VarDecl* variable) const;
    [[nodiscard]] const clang::Expr*
    enclosing_assignment_conditional_condition(const clang::CallExpr* call,
                                               const clang::VarDecl* variable) const;
    [[nodiscard]] bool call_is_forwarded(const clang::CallExpr* call) const;
    [[nodiscard]] bool call_is_discarded_expression(const clang::CallExpr* call) const;
    [[nodiscard]] bool call_is_operator(const clang::CallExpr* call) const;

    [[nodiscard]] CFGValueFlow* value_flow(const clang::FunctionDecl* function);
    [[nodiscard]] std::optional<CheckResult>
    analyze_flow_aliases(const clang::CallExpr* call, const Domain& domain);

    [[nodiscard]] bool call_returns_nullable_pointer(const clang::CallExpr* call) const;
    [[nodiscard]] NullStateAnalysis*
    null_state_analysis(const clang::FunctionDecl* function);
    void analyze_nullable_call(const clang::CallExpr* call);

    [[nodiscard]] std::string function_name(const clang::CallExpr* call) const;
    void emit(const clang::CallExpr* call, llvm::StringRef message,
              llvm::StringRef note = {}) const;
    [[nodiscard]] std::string missing_message(const std::vector<DomainValue>& missing) const;

    [[nodiscard]] CheckResult analyze_variable(const clang::CallExpr* call,
                                               const clang::VarDecl* variable,
                                               const Domain& domain);
    [[nodiscard]] CheckResult classify_call(const clang::CallExpr* call, const Domain& domain);
    [[nodiscard]] bool should_report(const CheckResult& result, const Domain& domain) const;
    void analyze_call(clang::CallExpr* call);

    clang::ASTContext& context_;
    clang::SourceManager& source_manager_;
    std::unordered_map<const clang::FunctionDecl*, Domain> domain_cache_;
    std::unordered_map<const clang::FunctionDecl*, std::unique_ptr<CFGValueFlow>> value_flow_cache_;
    std::unordered_set<const clang::FunctionDecl*> value_flow_failures_;
    std::unordered_map<const clang::FunctionDecl*, std::unique_ptr<NullStateAnalysis>>
        null_state_cache_;
    std::unordered_set<const clang::FunctionDecl*> null_state_failures_;
};

} // namespace returnguard::internal
