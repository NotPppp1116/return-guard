#include "status_api.h"

void complete_alias_check(void) {
    int status = annotated_status();
    int code = status;
    if (code == 1 || code == 4 || code == 32) {
        return;
    }
}

void partial_alias_check(void) {
    int status = annotated_status();
    int code = status;
    if (code == 1) {
        return;
    }
}

int alias_return_is_forwarded(void) {
    int status = annotated_status();
    int code = status;
    return code;
}

void overwritten_alias_is_not_a_check(void) {
    int status = annotated_status();
    int code = status;
    code = 1;
    if (code == 1 || code == 4 || code == 32) {
        return;
    }
}
