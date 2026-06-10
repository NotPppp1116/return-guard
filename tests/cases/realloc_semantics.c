#include <stdlib.h>

void realloc_failure_keeps_original(int* pointer) {
    int* replacement = (int*)realloc(pointer, sizeof(*pointer) * 2U);
    if (replacement == NULL) {
        *pointer = 1;
        return;
    }

    replacement[0] = 2;
    free(replacement);
}
