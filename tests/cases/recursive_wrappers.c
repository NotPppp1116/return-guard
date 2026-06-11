#include <stddef.h>

#define RG_NULLABLE __attribute__((annotate("returnguard.nullable")))

struct item { int value; };

RG_NULLABLE struct item* f(void);
RG_NULLABLE struct item* g(void) { return f(); }
RG_NULLABLE struct item* f(void) { return g(); }

int use_recursive(void) {
    return f()->value;
}
