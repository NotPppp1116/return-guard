#pragma once
#pragma clang system_header

namespace vendor {

inline int open(const char*, int) {
    return -1;
}

inline long send_like(int, const char*, unsigned long count) {
    return static_cast<long>(count);
}

inline long transfer_like(int, const char*, unsigned long count) {
    return static_cast<long>(count);
}

} // namespace vendor
