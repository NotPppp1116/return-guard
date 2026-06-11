#include <returnguard/Contracts.h>

int lower_level_status(void) RETURNGUARD_FAILS_NEGATIVE;

int lower_level_status(void) {
    return -1;
}

int propagate_status(void) {
    return lower_level_status();
}
