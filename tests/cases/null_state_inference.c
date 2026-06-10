#include <stddef.h>

#define RG_NULLABLE __attribute__((annotate("returnguard.nullable")))

struct item {
    int value;
};

RG_NULLABLE struct item* nullable_item(void);

struct item* safe_truthy_ternary(void) {
    static struct item fallback = {42};
    struct item* item = nullable_item();
    return item ? item : &fallback;
}

int use_safe_truthy_ternary(void) {
    return safe_truthy_ternary()->value;
}

struct item* safe_explicit_null_check_ternary(void) {
    static struct item fallback = {42};
    struct item* item = nullable_item();
    return item == NULL ? &fallback : item;
}

int use_safe_explicit_null_check_ternary(void) {
    return safe_explicit_null_check_ternary()->value;
}

struct item* unsafe_null_branch_ternary(void) {
    struct item* item = nullable_item();
    return item ? item : NULL;
}

int use_unsafe_null_branch_ternary(void) {
    return unsafe_null_branch_ternary()->value;
}

struct item* unsafe_nullable_call_branch(int selector) {
    static struct item fallback = {42};
    return selector ? nullable_item() : &fallback;
}

int use_unsafe_nullable_call_branch(int selector) {
    return unsafe_nullable_call_branch(selector)->value;
}
