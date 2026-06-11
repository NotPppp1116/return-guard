#include <stddef.h>

#define RG_NULLABLE __attribute__((annotate("returnguard.nullable")))

struct item { int value; };

RG_NULLABLE struct item* nullable_item(void);

int bool_binding_check(void) {
    struct item* p = nullable_item();
    const _Bool missing = (p == NULL);
    if (!missing) {
        return p->value;
    }
    return 0;
}
