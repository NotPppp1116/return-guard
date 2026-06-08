#include "DomainUtils.hpp"

#include <llvm/ADT/SmallString.h>

#include <algorithm>
#include <sstream>
#include <utility>

namespace returnguard::internal {

std::string apsint_to_string(const llvm::APSInt& value) {
    llvm::SmallString<32> buffer;
    value.toString(buffer, 10);
    return std::string(buffer);
}

bool same_value(const llvm::APSInt& lhs, const llvm::APSInt& rhs) {
    return llvm::APSInt::isSameValue(lhs, rhs);
}

void add_domain_value(Domain& domain, llvm::APSInt value, std::string label) {
    for (DomainValue& existing : domain.values) {
        if (!same_value(existing.value, value)) {
            continue;
        }

        if (!label.empty() &&
            std::find(existing.labels.begin(), existing.labels.end(), label) ==
                existing.labels.end()) {
            existing.labels.push_back(std::move(label));
        }
        return;
    }

    DomainValue entry{.value = std::move(value), .labels = {}};
    if (!label.empty()) {
        entry.labels.push_back(std::move(label));
    }
    domain.values.push_back(std::move(entry));
}

std::string value_display(const DomainValue& value) {
    if (value.labels.empty()) {
        return apsint_to_string(value.value);
    }

    std::ostringstream out;
    for (std::size_t index = 0; index < value.labels.size(); ++index) {
        if (index != 0U) {
            out << '/';
        }
        out << value.labels[index];
    }
    return out.str();
}

} // namespace returnguard::internal
