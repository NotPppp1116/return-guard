#include <stdlib.h>

// 1. Stack Return
int* return_stack_var(void) {
    int local = 42;
    return &local; // Should warn!
}

int* return_stack_array(void) {
    int array[5];
    return array; // Should warn!
}

int* return_stack_ptr(void) {
    int local = 42;
    int* ptr = &local;
    return ptr; // Should warn!
}

// 2. Out of Bounds
int oob_access_positive(void) {
    int array[5] = {1, 2, 3, 4, 5};
    return array[5]; // Should warn!
}

int oob_access_negative(void) {
    int array[5] = {1, 2, 3, 4, 5};
    return array[-1]; // Should warn!
}

int oob_access_safe(void) {
    int array[5] = {1, 2, 3, 4, 5};
    return array[4]; // Safe!
}

// 3. Use-After-Free
int uaf_simple(void) {
    int* ptr = (int*)malloc(sizeof(int));
    if (ptr == NULL) return 0;
    *ptr = 42;
    free(ptr);
    return *ptr; // Should warn!
}

int uaf_reassigned_safe(void) {
    int* ptr = (int*)malloc(sizeof(int));
    if (ptr == NULL) return 0;
    free(ptr);
    ptr = (int*)malloc(sizeof(int));
    if (ptr == NULL) return 0;
    *ptr = 42; // Safe!
    free(ptr);
    return 0;
}
