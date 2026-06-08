#include "IfChain.hpp"

#include <clang/AST/Stmt.h>
#include <llvm/Support/Casting.h>

namespace returnguard::internal {

const clang::IfStmt* final_else_if(const clang::IfStmt* statement) {
    const clang::IfStmt* current = statement;
    while (current != nullptr) {
        const clang::Stmt* else_statement = current->getElse();
        const auto* next = llvm::dyn_cast_or_null<clang::IfStmt>(else_statement);
        if (next == nullptr) {
            return current;
        }
        current = next;
    }
    return nullptr;
}

bool has_final_else(const clang::IfStmt* statement) {
    const clang::IfStmt* last = final_else_if(statement);
    return last != nullptr && last->getElse() != nullptr;
}

std::vector<const clang::IfStmt*> if_chain(const clang::IfStmt* statement) {
    std::vector<const clang::IfStmt*> result;
    const clang::IfStmt* current = statement;
    while (current != nullptr) {
        result.push_back(current);
        current = llvm::dyn_cast_or_null<clang::IfStmt>(current->getElse());
    }
    return result;
}

} // namespace returnguard::internal
