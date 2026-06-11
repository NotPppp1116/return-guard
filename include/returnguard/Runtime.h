#pragma once

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#if defined(__cplusplus)
#include <type_traits>
#endif

#if defined(__GNUC__) || defined(__clang__)
#define RETURNGUARD_RUNTIME_NORETURN __attribute__((noreturn))
#define RETURNGUARD_RUNTIME_COLD __attribute__((cold))
#define RETURNGUARD_RUNTIME_NOINLINE __attribute__((noinline))
#define RETURNGUARD_RUNTIME_HIDDEN __attribute__((visibility("hidden")))
#define RETURNGUARD_RUNTIME_WEAK __attribute__((weak))
#elif defined(__cplusplus)
#define RETURNGUARD_RUNTIME_NORETURN [[noreturn]]
#define RETURNGUARD_RUNTIME_COLD
#define RETURNGUARD_RUNTIME_NOINLINE
#define RETURNGUARD_RUNTIME_HIDDEN
#define RETURNGUARD_RUNTIME_WEAK
#else
#define RETURNGUARD_RUNTIME_NORETURN _Noreturn
#define RETURNGUARD_RUNTIME_COLD
#define RETURNGUARD_RUNTIME_NOINLINE
#define RETURNGUARD_RUNTIME_HIDDEN
#define RETURNGUARD_RUNTIME_WEAK
#endif

#if defined(__cplusplus) && __cplusplus >= 201703L
#define RETURNGUARD_RUNTIME_NODISCARD [[nodiscard]]
#elif defined(__GNUC__) || defined(__clang__)
#define RETURNGUARD_RUNTIME_NODISCARD __attribute__((warn_unused_result))
#else
#define RETURNGUARD_RUNTIME_NODISCARD
#endif

#if defined(__cplusplus)
#define RETURNGUARD_RUNTIME_NOEXCEPT noexcept
extern "C" {
#else
#define RETURNGUARD_RUNTIME_NOEXCEPT
#endif

typedef enum ReturnGuardSecretResult {
    RETURNGUARD_SECRET_OK = 0,
    RETURNGUARD_SECRET_INVALID = -1,
    RETURNGUARD_SECRET_REGISTRY_FULL = -2,
    RETURNGUARD_SECRET_NOT_FOUND = -3,
    RETURNGUARD_SECRET_BUSY = -4,
    RETURNGUARD_SECRET_ALREADY_REGISTERED = -5,
} ReturnGuardSecretResult;

/*
 * Register memory that should be zeroed before the fatal hook runs. Register a
 * region before placing sensitive bytes in it, and unregister it before the
 * region is freed or otherwise becomes invalid. The registry has fixed
 * capacity and never allocates.
 */
RETURNGUARD_RUNTIME_NODISCARD ReturnGuardSecretResult
returnguard_register_secret(void* memory, size_t size) RETURNGUARD_RUNTIME_NOEXCEPT;
RETURNGUARD_RUNTIME_NODISCARD ReturnGuardSecretResult
returnguard_unregister_secret(void* memory) RETURNGUARD_RUNTIME_NOEXCEPT;

RETURNGUARD_RUNTIME_NORETURN RETURNGUARD_RUNTIME_COLD RETURNGUARD_RUNTIME_NOINLINE
    RETURNGUARD_RUNTIME_HIDDEN void
    __rg_fatal(uint64_t site_id, int saved_errno) RETURNGUARD_RUNTIME_NOEXCEPT;

/*
 * Applications may provide a strong definition of this hook. Registered secret
 * regions have already been wiped when it runs. The default weak implementation
 * does nothing. The hook may emit a minimal diagnostic, but it must not attempt
 * to recover execution.
 */
void __rg_fatal_hook(uint64_t site_id, int saved_errno) RETURNGUARD_RUNTIME_NOEXCEPT;

#if defined(__cplusplus)
}

namespace returnguard_runtime_detail {

template <typename T>
inline T check_negative(T result, uint64_t site_id) noexcept {
    static_assert(std::is_integral_v<T> && std::is_signed_v<T>,
                  "ReturnGuard negative-result checks require a signed integer type");
    if (result < 0) {
        const int saved_errno = errno;
        __rg_fatal(site_id, saved_errno);
    }
    return result;
}

template <typename T>
inline T* check_null(T* result, uint64_t site_id) noexcept {
    if (result == nullptr) {
        const int saved_errno = errno;
        __rg_fatal(site_id, saved_errno);
    }
    return result;
}

} // namespace returnguard_runtime_detail

#define __RG_CHECK_NEGATIVE(expression, site_id)                                             \
    (::returnguard_runtime_detail::check_negative((expression), (uint64_t)(site_id)))
#define __RG_CHECK_NULL(expression, site_id)                                                 \
    (::returnguard_runtime_detail::check_null((expression), (uint64_t)(site_id)))

#elif defined(__GNUC__) || defined(__clang__)

/*
 * ReturnGuard itself is Clang-based, so generated C may use the reserved GNU
 * spellings supported by both Clang and GCC. __extension__ suppresses pedantic
 * diagnostics while the statement expression and __auto_type preserve the
 * exact return type and evaluate the wrapped call exactly once.
 */
#define __RG_CHECK_NEGATIVE(expression, site_id)                                             \
    __extension__({                                                                          \
        __auto_type __rg_result = (expression);                                               \
        if (__rg_result < 0) {                                                               \
            const int __rg_saved_errno = errno;                                              \
            __rg_fatal((uint64_t)(site_id), __rg_saved_errno);                               \
        }                                                                                    \
        __rg_result;                                                                         \
    })

#define __RG_CHECK_NULL(expression, site_id)                                                 \
    __extension__({                                                                          \
        __auto_type __rg_result = (expression);                                               \
        if (__rg_result == NULL) {                                                           \
            const int __rg_saved_errno = errno;                                              \
            __rg_fatal((uint64_t)(site_id), __rg_saved_errno);                               \
        }                                                                                    \
        __rg_result;                                                                         \
    })

#else

static inline signed char __rg_check_negative_schar(signed char result, uint64_t site_id) {
    if (result < 0) {
        const int saved_errno = errno;
        __rg_fatal(site_id, saved_errno);
    }
    return result;
}

static inline short __rg_check_negative_short(short result, uint64_t site_id) {
    if (result < 0) {
        const int saved_errno = errno;
        __rg_fatal(site_id, saved_errno);
    }
    return result;
}

static inline int __rg_check_negative_int(int result, uint64_t site_id) {
    if (result < 0) {
        const int saved_errno = errno;
        __rg_fatal(site_id, saved_errno);
    }
    return result;
}

static inline long __rg_check_negative_long(long result, uint64_t site_id) {
    if (result < 0) {
        const int saved_errno = errno;
        __rg_fatal(site_id, saved_errno);
    }
    return result;
}

static inline long long __rg_check_negative_llong(long long result, uint64_t site_id) {
    if (result < 0) {
        const int saved_errno = errno;
        __rg_fatal(site_id, saved_errno);
    }
    return result;
}

static inline void* __rg_check_null_pointer(void* result, uint64_t site_id) {
    if (result == NULL) {
        const int saved_errno = errno;
        __rg_fatal(site_id, saved_errno);
    }
    return result;
}

#define __RG_CHECK_NEGATIVE(expression, site_id)                                             \
    _Generic((expression),                                                                  \
        signed char: __rg_check_negative_schar,                                              \
        short: __rg_check_negative_short,                                                    \
        int: __rg_check_negative_int,                                                        \
        long: __rg_check_negative_long,                                                      \
        long long: __rg_check_negative_llong)((expression), (uint64_t)(site_id))

#define __RG_CHECK_NULL(expression, site_id)                                                 \
    __rg_check_null_pointer((void*)(expression), (uint64_t)(site_id))

#endif
