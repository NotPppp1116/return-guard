#include <stddef.h>

void mutate_boolean(_Bool*);
#define RG_NULLABLE __attribute__((annotate("returnguard.nullable")))
struct item { int value; };
RG_NULLABLE struct item* nullable_item(void);

int bool_address_escape(void) {
    struct item* p = nullable_item();
    _Bool safe = (p != NULL);
    mutate_boolean(&safe);
    if (safe) {
        return p->value;
    }
    return 0;
}
