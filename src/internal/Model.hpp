#pragma once

#include <llvm/ADT/APSInt.h>

#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace clang {
class Expr;
}

namespace returnguard::internal {

using ExpressionSet = std::unordered_set<const clang::Expr*>;

enum class Truth {
    False,
    True,
    Unknown,
};

enum class HandlingKind {
    Ignored,
    ExplicitlyIgnored,
    Forwarded,
    Consumed,
    PartiallyChecked,
    ExhaustivelyChecked,
};

struct DomainValue {
    llvm::APSInt value;
    std::vector<std::string> labels;
};

struct Domain {
    bool finite = false;
    bool inferred_from_body = false;
    std::string type_name;
    std::vector<DomainValue> values;
};

struct CheckResult {
    HandlingKind kind = HandlingKind::Ignored;
    std::vector<DomainValue> missing;
    std::string detail;
};

struct SymbolicInteger {
    bool is_target = false;
    std::optional<llvm::APSInt> constant;
};

} // namespace returnguard::internal
