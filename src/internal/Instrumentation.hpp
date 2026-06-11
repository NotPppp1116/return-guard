#pragma once

#include "ContractPolicy.hpp"
#include "Model.hpp"
#include "SiteMetadata.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace clang {
class ASTContext;
class CallExpr;
class Rewriter;
}

namespace returnguard::internal {

class Instrumentation final {
  public:
    Instrumentation(clang::ASTContext& context,
                    clang::Rewriter& rewriter,
                    std::vector<SiteMetadata>& sites);

    [[nodiscard]] bool consider(const clang::CallExpr* call,
                                const CheckResult& handling);

  private:
    [[nodiscard]] bool should_instrument(const CheckResult& handling) const;
    [[nodiscard]] bool validate_contract(const clang::CallExpr* call,
                                         FailurePredicate predicate) const;
    [[nodiscard]] bool wrap_call(const clang::CallExpr* call,
                                 FailurePredicate predicate);
    [[nodiscard]] std::optional<SiteMetadata>
    metadata_for_call(const clang::CallExpr* call,
                      FailurePredicate predicate);
    [[nodiscard]] std::string normalized_site_path(const clang::CallExpr* call) const;
    [[nodiscard]] std::string enclosing_function_name(const clang::CallExpr* call) const;
    [[nodiscard]] bool ensure_runtime_header();

    clang::ASTContext& context_;
    clang::Rewriter& rewriter_;
    std::vector<SiteMetadata>& sites_;
    std::unordered_map<std::uint64_t, std::string> known_site_ids_;
    bool runtime_header_inserted_ = false;
};

} // namespace returnguard::internal
