#pragma once

#include "Model.hpp"

#include <llvm/ADT/APSInt.h>

#include <string>

namespace returnguard::internal {

[[nodiscard]] std::string apsint_to_string(const llvm::APSInt& value);
[[nodiscard]] bool same_value(const llvm::APSInt& lhs, const llvm::APSInt& rhs);
void add_domain_value(Domain& domain, llvm::APSInt value, std::string label);
[[nodiscard]] std::string value_display(const DomainValue& value);

} // namespace returnguard::internal
