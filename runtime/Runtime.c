#include <returnguard/Runtime.h>

#include <stdatomic.h>
#include <stdlib.h>

#define RETURNGUARD_SECRET_CAPACITY 64U

typedef enum SecretSlotState {
    SECRET_SLOT_FREE = 0U,
    SECRET_SLOT_UPDATING = 1U,
    SECRET_SLOT_READY = 2U,
    SECRET_SLOT_WIPING = 3U,
} SecretSlotState;

typedef struct SecretSlot {
    atomic_uint state;
    void* memory;
    size_t size;
} SecretSlot;

static SecretSlot secret_slots[RETURNGUARD_SECRET_CAPACITY];
static atomic_flag secret_registry_lock = ATOMIC_FLAG_INIT;

static void lock_secret_registry(void) {
    while (atomic_flag_test_and_set_explicit(
        &secret_registry_lock, memory_order_acquire)) {
    }
}

static void unlock_secret_registry(void) {
    atomic_flag_clear_explicit(&secret_registry_lock, memory_order_release);
}

static void secure_zero_memory(void* memory, size_t size) {
    volatile unsigned char* bytes = (volatile unsigned char*)memory;
    while (size > 0U) {
        *bytes = 0U;
        ++bytes;
        --size;
    }
    atomic_signal_fence(memory_order_seq_cst);
}

ReturnGuardSecretResult returnguard_register_secret(void* memory, size_t size) {
    if (memory == NULL || size == 0U) {
        return RETURNGUARD_SECRET_INVALID;
    }

    lock_secret_registry();

    SecretSlot* free_slot = NULL;
    for (size_t index = 0U; index < RETURNGUARD_SECRET_CAPACITY; ++index) {
        SecretSlot* slot = &secret_slots[index];
        const unsigned state =
            atomic_load_explicit(&slot->state, memory_order_acquire);

        if ((state == SECRET_SLOT_READY || state == SECRET_SLOT_WIPING) &&
            slot->memory == memory) {
            unlock_secret_registry();
            return state == SECRET_SLOT_READY
                       ? RETURNGUARD_SECRET_ALREADY_REGISTERED
                       : RETURNGUARD_SECRET_BUSY;
        }
        if (state == SECRET_SLOT_FREE && free_slot == NULL) {
            free_slot = slot;
        }
    }

    if (free_slot == NULL) {
        unlock_secret_registry();
        return RETURNGUARD_SECRET_REGISTRY_FULL;
    }

    atomic_store_explicit(
        &free_slot->state, SECRET_SLOT_UPDATING, memory_order_relaxed);
    free_slot->memory = memory;
    free_slot->size = size;
    atomic_store_explicit(
        &free_slot->state, SECRET_SLOT_READY, memory_order_release);

    unlock_secret_registry();
    return RETURNGUARD_SECRET_OK;
}

ReturnGuardSecretResult returnguard_unregister_secret(void* memory) {
    if (memory == NULL) {
        return RETURNGUARD_SECRET_INVALID;
    }

    lock_secret_registry();

    for (size_t index = 0U; index < RETURNGUARD_SECRET_CAPACITY; ++index) {
        SecretSlot* slot = &secret_slots[index];
        const unsigned state =
            atomic_load_explicit(&slot->state, memory_order_acquire);

        if ((state != SECRET_SLOT_READY && state != SECRET_SLOT_WIPING) ||
            slot->memory != memory) {
            continue;
        }
        if (state == SECRET_SLOT_WIPING) {
            unlock_secret_registry();
            return RETURNGUARD_SECRET_BUSY;
        }

        unsigned expected = SECRET_SLOT_READY;
        if (!atomic_compare_exchange_strong_explicit(
                &slot->state,
                &expected,
                SECRET_SLOT_UPDATING,
                memory_order_acq_rel,
                memory_order_acquire)) {
            if (expected == SECRET_SLOT_WIPING) {
                unlock_secret_registry();
                return RETURNGUARD_SECRET_BUSY;
            }
            continue;
        }

        slot->memory = NULL;
        slot->size = 0U;
        atomic_store_explicit(
            &slot->state, SECRET_SLOT_FREE, memory_order_release);
        unlock_secret_registry();
        return RETURNGUARD_SECRET_OK;
    }

    unlock_secret_registry();
    return RETURNGUARD_SECRET_NOT_FOUND;
}

static void wipe_registered_secrets(void) {
    for (size_t index = 0U; index < RETURNGUARD_SECRET_CAPACITY; ++index) {
        SecretSlot* slot = &secret_slots[index];
        unsigned expected = SECRET_SLOT_READY;
        if (!atomic_compare_exchange_strong_explicit(
                &slot->state,
                &expected,
                SECRET_SLOT_WIPING,
                memory_order_acq_rel,
                memory_order_acquire)) {
            continue;
        }

        if (slot->memory != NULL && slot->size > 0U) {
            secure_zero_memory(slot->memory, slot->size);
        }
    }
}

RETURNGUARD_RUNTIME_WEAK void __rg_fatal_hook(uint32_t site_id, int saved_errno) {
    (void)site_id;
    (void)saved_errno;
}

RETURNGUARD_RUNTIME_NORETURN RETURNGUARD_RUNTIME_COLD RETURNGUARD_RUNTIME_NOINLINE
    RETURNGUARD_RUNTIME_HIDDEN void
    __rg_fatal(uint32_t site_id, int saved_errno) {
    static atomic_flag failure_in_progress = ATOMIC_FLAG_INIT;

    if (!atomic_flag_test_and_set_explicit(
            &failure_in_progress, memory_order_relaxed)) {
        wipe_registered_secrets();
        __rg_fatal_hook(site_id, saved_errno);
    }

    _Exit(127);
}
