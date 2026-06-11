#include <stddef.h>

_Bool cond(void);
#define RG_NULLABLE __attribute__((annotate("returnguard.nullable")))
struct item { int value; };
RG_NULLABLE struct item* nullable_item(void);

int bool_multiple_assigns_join(void) {
    struct item* p = nullable_item();
    _Bool safe = (p != NULL);
    if (cond()) {
        safe = 1;
    } else {
        safe = 0;
    }
    if (safe) {
        return p->value;
    }
    return 0;
}
