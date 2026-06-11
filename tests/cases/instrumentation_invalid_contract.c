#include <returnguard/Contracts.h>

unsigned bad_negative_contract(void) RETURNGUARD_FAILS_NEGATIVE;
int bad_null_contract(void) RETURNGUARD_FAILS_NULL;

unsigned bad_negative_contract(void) {
    return 0U;
}

int bad_null_contract(void) {
    return 1;
}

int main(void) {
    (void)bad_negative_contract();
    (void)bad_null_contract();
    return 0;
}
