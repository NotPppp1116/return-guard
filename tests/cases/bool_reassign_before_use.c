#include <stddef.h>

_Bool unrelated_condition(void);
#define RG_NULLABLE __attribute__((annotate("returnguard.nullable")))
struct item { int value; };
RG_NULLABLE struct item* nullable_item(void);

int bool_reassign_before_use(void) {
    struct item* p = nullable_item();
    _Bool safe = (p != NULL);
    safe = unrelated_condition();
    if (safe) {
        return p->value;
    }
    return 0;
}
