#include <stddef.h>
#include <stdlib.h>

int double_free_and_uaf(void) {
    int* p = (int*)malloc(sizeof(int));
    if (p == NULL)
        return 0;
    free(p);
    int x = *p;
    free(p);
    return x;
}
