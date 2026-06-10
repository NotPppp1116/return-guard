#include <stddef.h>

char* nullable_bytes(void) {
    return NULL;
}

int unchecked_index(int idx) {
    char* b = nullable_bytes();
    return b[idx];
}

int checked_index(int idx) {
    char* b = nullable_bytes();
    if (b == NULL)
        return 0;
    return b[idx];
}
