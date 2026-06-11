#include "NullStateAnalysis.hpp"

#include "AstUtils.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/SourceManager.h>
#include <llvm/Support/Casting.h>

#include <algorithm>
#include <deque>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace returnguard::internal {
namespace {

#include "NullStateModel.inc"
#include "NullStateTransfer.inc"

} // namespace

#include "NullStateDriver.inc"

} // namespace returnguard::internal
