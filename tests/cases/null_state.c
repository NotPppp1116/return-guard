#include <stddef.h>

#define RG_NULLABLE __attribute__((annotate("returnguard.nullable")))

struct item {
    int value;
};

RG_NULLABLE struct item* nullable_item(void);
RG_NULLABLE char* nullable_bytes(void);
typedef int (*callback)(void);
RG_NULLABLE callback nullable_callback(void);
struct item* _Nullable native_nullable_item(void);

int unchecked_arrow(void) {
    struct item* item = nullable_item();
    return item->value;
}

int unchecked_direct(void) {
    return nullable_item()->value;
}

int unchecked_subscript(void) {
    char* bytes = nullable_bytes();
    return bytes[0];
}

int checked_too_late(void) {
    struct item* item = nullable_item();
    int value = item->value;
    if (item == NULL) {
        return 0;
    }
    return value;
}

int guarded_truthy(void) {
    struct item* item = nullable_item();
    if (item) {
        return item->value;
    }
    return 0;
}

int guarded_early_return(void) {
    struct item* item = nullable_item();
    if (!item) {
        return 0;
    }
    return item->value;
}

int guarded_equal_null(void) {
    struct item* item = nullable_item();
    if (item == NULL) {
        return 0;
    }
    return (*item).value;
}

int guarded_through_alias(void) {
    struct item* item = nullable_item();
    struct item* alias = item;
    if (alias == NULL) {
        return 0;
    }
    return item->value + alias->value;
}

int safe_short_circuit_and(void) {
    struct item* item = nullable_item();
    return item && item->value;
}

int safe_short_circuit_inverse_or(void) {
    struct item* item = nullable_item();
    return !item || item->value;
}

int unsafe_short_circuit_or(void) {
    struct item* item = nullable_item();
    return item || item->value;
}

int reassigned_before_use(void) {
    struct item fallback = {42};
    struct item* item = nullable_item();
    item = &fallback;
    return item->value;
}

int replaced_on_null(void) {
    struct item fallback = {42};
    struct item* item = nullable_item();
    if (item == NULL) {
        item = &fallback;
    }
    return item->value;
}

int guarded_conditional_origin(int choose_nullable) {
    struct item fallback = {42};
    struct item* item = choose_nullable ? nullable_item() : &fallback;
    if (item == NULL) {
        return 0;
    }
    return item->value;
}

int unchecked_conditional_origin(int choose_nullable) {
    struct item fallback = {42};
    struct item* item = choose_nullable ? nullable_item() : &fallback;
    return item->value;
}

int guarded_conditional_alias(int choose_nullable) {
    struct item fallback = {42};
    struct item* item = nullable_item();
    struct item* alias = choose_nullable ? item : &fallback;
    if (alias == NULL) {
        return 0;
    }
    return alias->value;
}

int unchecked_original_after_conditional_alias(int choose_fallback) {
    struct item fallback = {42};
    struct item* item = nullable_item();
    struct item* alias = choose_fallback ? &fallback : item;
    if (alias != NULL) {
        return item->value;
    }
    return 0;
}

int unchecked_native_nullability(void) {
    struct item* item = native_nullable_item();
    return item->value;
}

int unchecked_nullable_callback(void) {
    callback function = nullable_callback();
    return function();
}

struct item* check_and_return_safe(void) {
    static struct item fallback = {42};
    struct item* item = nullable_item();
    if (item == NULL) {
        return &fallback;
    }
    return item;
}

int unchecked_safe_inferred(void) {
    struct item* item = check_and_return_safe();
    return item->value;
}

struct item* check_and_return_unsafe(void) {
    struct item* item = nullable_item();
    return item;
}

int unchecked_unsafe_inferred(void) {
    struct item* item = check_and_return_unsafe();
    return item->value;
}


