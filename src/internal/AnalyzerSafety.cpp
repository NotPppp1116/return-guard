#include "Analyzer.hpp"

#include <clang/AST/Decl.h>

namespace returnguard::internal {

void Analyzer::analyze_safety(const clang::FunctionDecl*) {}

} // namespace returnguard::internal
