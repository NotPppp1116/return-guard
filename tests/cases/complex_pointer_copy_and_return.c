#include <stddef.h>

int* nullable_intptr(void) {
    return NULL;
}

int* copy_and_return(void) {
    int* p = nullable_intptr();
    int* q = p;
    return q;
}

int use_copied_return(void) {
    int* p = copy_and_return();
    return *p;
}
