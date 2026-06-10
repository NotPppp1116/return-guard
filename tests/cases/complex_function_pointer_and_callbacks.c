#include <stddef.h>

typedef int (*callback)(int);

callback nullable_cb(void) {
    return NULL;
}

int use_nullable_cb(void) {
    callback cb = nullable_cb();
    return cb(42);
}
