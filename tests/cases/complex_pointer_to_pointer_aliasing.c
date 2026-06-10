#include <stddef.h>

int* nullable_intptr(void) {
    return NULL;
}

int pointer_to_pointer_aliasing(void) {
    int* p = nullable_intptr();
    int** pp = &p;
    if (*pp == NULL)
        return 0;
    return **pp;
}
