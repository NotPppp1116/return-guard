#include <stddef.h>

void* custom_alloc(size_t size) __attribute__((malloc));
void* guaranteed_alloc(size_t size)
    __attribute__((malloc, returns_nonnull));

int unchecked_allocator(void) {
    int* pointer = (int*)custom_alloc(sizeof(int));
    return *pointer;
}

int checked_allocator(void) {
    int* pointer = (int*)custom_alloc(sizeof(int));
    if (pointer == NULL) {
        return 0;
    }
    return *pointer;
}

int guaranteed_allocator_use(void) {
    int* pointer = (int*)guaranteed_alloc(sizeof(int));
    return *pointer;
}

static int* nullable_a(int value);

static int* nullable_b(int value) {
    if (value <= 0) {
        return NULL;
    }
    return nullable_a(value - 1);
}

static int* nullable_a(int value) {
    return nullable_b(value);
}

int unchecked_recursive_nullable(int value) {
    int* pointer = nullable_a(value);
    return *pointer;
}

static int domain_a(int value);

static int domain_b(int value) {
    if (value <= 0) {
        return 2;
    }
    return domain_a(value - 1);
}

static int domain_a(int value) {
    if (value <= 0) {
        return 1;
    }
    return domain_b(value - 1);
}

void exhaustive_recursive_domain(int value) {
    int status = domain_a(value);
    if (status == 1 || status == 2) {
        return;
    }
}

void incomplete_recursive_domain(int value) {
    int status = domain_a(value);
    if (status == 1) {
        return;
    }
}
