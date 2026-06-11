#pragma once
#pragma clang system_header

namespace vendor {

inline int open(const char*, int) {
    return -1;
}

} // namespace vendor
