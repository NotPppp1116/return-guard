#include <returnguard/Contracts.h>

struct item {
    int value;
};

struct item* annotated_failure(void) RETURNGUARD_FAILS_NULL;
struct item* configured_failure(void);

int unchecked_annotated_failure_contract(void) {
    return annotated_failure()->value;
}

int unchecked_configured_failure_contract(void) {
    return configured_failure()->value;
}

int checked_configured_failure_contract(void) {
    struct item* item = configured_failure();
    if (item == 0) {
        return 0;
    }
    return item->value;
}
