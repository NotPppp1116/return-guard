#include <stddef.h>

#define RG_NULLABLE __attribute__((annotate("returnguard.nullable")))

struct item { int value; };

RG_NULLABLE struct item* nullable_item(void);

int bool_negated_init_safe(void) {
    struct item* p = nullable_item();
    const _Bool safe = (p == NULL);
    if (!safe) {
        return p->value;
    }
    return 0;
}
