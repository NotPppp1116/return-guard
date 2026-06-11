#include <stddef.h>

#define RG_NULLABLE __attribute__((annotate("returnguard.nullable")))

struct item { int value; };

RG_NULLABLE struct item* a(void);
struct item* wrapper1(void) { return a(); }
struct item* wrapper2(void) { return wrapper1(); }

int use_wrapper2(void) {
    return wrapper2()->value;
}
