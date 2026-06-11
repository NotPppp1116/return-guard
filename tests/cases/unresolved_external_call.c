#include <stddef.h>

#define RG_NULLABLE __attribute__((annotate("returnguard.nullable")))

struct item { int value; };

RG_NULLABLE struct item* external_func(void);
struct item* ext_wrapper(void) { return external_func(); }

int use_ext_wrapper(void) {
    return ext_wrapper()->value;
}
