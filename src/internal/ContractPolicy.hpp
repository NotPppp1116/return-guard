#pragma once

#include <optional>

namespace clang {
class FunctionDecl;
class SourceManager;
} // namespace clang

namespace returnguard::internal {

enum class FailurePredicate {
    Null,
    Negative,
};

[[nodiscard]] std::optional<FailurePredicate>
failure_contract(const clang::FunctionDecl& function, const clang::SourceManager& source_manager);

[[nodiscard]] std::optional<unsigned>
byte_count_parameter_index(const clang::FunctionDecl& function,
                           const clang::SourceManager& source_manager);

} // namespace returnguard::internal
