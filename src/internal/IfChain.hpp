#pragma once

#include <vector>

namespace clang {
class IfStmt;
}

namespace returnguard::internal {

[[nodiscard]] const clang::IfStmt* final_else_if(
    const clang::IfStmt* statement);
[[nodiscard]] bool has_final_else(const clang::IfStmt* statement);
[[nodiscard]] std::vector<const clang::IfStmt*> if_chain(
    const clang::IfStmt* statement);

} // namespace returnguard::internal
