#include <stddef.h>
#include <string.h>

int* nullable_intptr(void) {
    return NULL;
}

int memcpy_aliasing(void) {
    int* p = nullable_intptr();
    int* q;
    memcpy(&q, &p, sizeof(q));
    return *q;
}
