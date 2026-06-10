#include <returnguard/Runtime.h>

#include <stdatomic.h>
#include <stdlib.h>

RETURNGUARD_RUNTIME_WEAK void __rg_fatal_hook(uint32_t site_id, int saved_errno) {
    (void)site_id;
    (void)saved_errno;
}

RETURNGUARD_RUNTIME_NORETURN RETURNGUARD_RUNTIME_COLD RETURNGUARD_RUNTIME_NOINLINE
    RETURNGUARD_RUNTIME_HIDDEN void
    __rg_fatal(uint32_t site_id, int saved_errno) {
    static atomic_flag failure_in_progress = ATOMIC_FLAG_INIT;

    if (!atomic_flag_test_and_set_explicit(&failure_in_progress, memory_order_relaxed)) {
        __rg_fatal_hook(site_id, saved_errno);
    }

    _Exit(127);
}
