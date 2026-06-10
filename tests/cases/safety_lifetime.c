#include <stddef.h>
#include <stdlib.h>
#include <string.h>

void sink_int_ptr(int* value);

int alias_free_deref(void) {
    int* p = (int*)malloc(sizeof(int));
    if (p == NULL) return 0;
    int* q = p;
    free(p);
    return *q;
}

int branch_maybe_free(int selector) {
    int* p = (int*)malloc(sizeof(int));
    if (p == NULL) return 0;
    if (selector) {
        free(p);
    }
    return *p;
}

int reassigned_after_free_is_safe(void) {
    int* p = (int*)malloc(sizeof(int));
    if (p == NULL) return 0;
    free(p);
    p = (int*)malloc(sizeof(int));
    if (p == NULL) return 0;
    int value = *p;
    free(p);
    return value;
}

int double_free_through_alias(void) {
    int* p = (int*)malloc(sizeof(int));
    if (p == NULL) return 0;
    int* q = p;
    free(p);
    free(q);
    return 0;
}

int memcpy_freed_alias(void) {
    int* p = (int*)malloc(sizeof(int));
    if (p == NULL) return 0;
    int* q;
    memcpy(&q, &p, sizeof(q));
    free(p);
    return *q;
}

int pass_freed_to_function(void) {
    int* p = (int*)malloc(sizeof(int));
    if (p == NULL) return 0;
    free(p);
    sink_int_ptr(p);
    return 0;
}
