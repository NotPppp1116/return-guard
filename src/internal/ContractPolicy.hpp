#pragma once

#include <optional>

namespace clang {
class FunctionDecl;
class SourceManager;
}

namespace returnguard::internal {

enum class FailurePredicate {
    Null,
    Negative,
};

[[nodiscard]] std::optional<FailurePredicate>
failure_contract(const clang::FunctionDecl& function,
                 const clang::SourceManager& source_manager);

} // namespace returnguard::internal
