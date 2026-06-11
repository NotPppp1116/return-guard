#pragma once

#include <cstdint>
#include <string>

namespace returnguard::internal {

struct SiteMetadata {
    std::uint64_t id = 0U;
    std::string file;
    unsigned line = 0U;
    unsigned column = 0U;
    std::string function;
    std::string callee;
    std::string predicate;
};

} // namespace returnguard::internal
