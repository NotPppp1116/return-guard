#include <stddef.h>

struct leaf {
    int value;
};
struct mid {
    struct leaf* leaf;
};
struct top {
    struct mid* m;
};

struct top* nullable_top(void) {
    return NULL;
}

int unchecked_nested_member(void) {
    struct top* t = nullable_top();
    return t->m->leaf->value;
}

int guarded_nested_member(void) {
    struct top* t = nullable_top();
    if (!t || !t->m)
        return 0;
    return t->m->leaf->value;
}
