#pragma once

#include "Model.hpp"

#include <cstdint>

namespace clang {
class ASTContext;
class CallExpr;
class Rewriter;
}

namespace returnguard::internal {

class Instrumentation final {
  public:
    Instrumentation(clang::ASTContext& context, clang::Rewriter& rewriter);

    [[nodiscard]] bool consider(const clang::CallExpr* call,
                                const CheckResult& handling);

  private:
    enum class FailurePredicate {
        Null,
        Negative,
    };

    [[nodiscard]] bool should_instrument(const CheckResult& handling) const;
    [[nodiscard]] bool wrap_call(const clang::CallExpr* call,
                                 FailurePredicate predicate);
    [[nodiscard]] std::uint32_t site_id(const clang::CallExpr* call) const;
    [[nodiscard]] bool ensure_runtime_header();

    clang::ASTContext& context_;
    clang::Rewriter& rewriter_;
    bool runtime_header_inserted_ = false;
};

} // namespace returnguard::internal
