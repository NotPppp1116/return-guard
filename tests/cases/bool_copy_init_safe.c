#include <stddef.h>

#define RG_NULLABLE __attribute__((annotate("returnguard.nullable")))

struct item { int value; };

RG_NULLABLE struct item* nullable_item(void);

int bool_copy_init_safe(void) {
    struct item* p = nullable_item();
    const _Bool a = (p != NULL);
    const _Bool b = a;
    if (b) {
        return p->value;
    }
    return 0;
}
