#include <stddef.h>
#include <stdlib.h>
#include <string.h>

void sink_int_ptr(int* value);
int* external_int_ptr(void);

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

int parameter_uaf(int* p) {
    free(p);
    return *p;
}

int parameter_double_free(int* p) {
    free(p);
    free(p);
    return 0;
}

int unknown_return_uaf(void) {
    int* p = external_int_ptr();
    free(p);
    return *p;
}

int realloc_assignment_tracks_new_value(int* p) {
    p = (int*)realloc(p, sizeof(int));
    if (p == NULL) return 0;
    free(p);
    return *p;
}
