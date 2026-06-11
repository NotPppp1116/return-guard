#include <stddef.h>

_Bool some_cond(void);
#define RG_NULLABLE __attribute__((annotate("returnguard.nullable")))
struct item { int value; };
RG_NULLABLE struct item* nullable_item(void);

int bool_assign_one_branch(void) {
    struct item* p = nullable_item();
    _Bool safe = (p != NULL);
    if (some_cond()) {
        safe = 1;
    }
    if (safe) {
        return p->value;
    }
    return 0;
}
