#pragma once

#include <llvm/ADT/APSInt.h>

#include <optional>
#include <string>
#include <vector>

namespace returnguard::internal {

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
