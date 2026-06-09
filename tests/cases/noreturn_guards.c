#include "status_api.h"

void direct_noreturn_guard(void) {
    if (annotated_status() != 1) {
        fatal_status(1);
    }
}

void stored_noreturn_guard(void) {
    int status = annotated_status();
    if (status != 1) {
        fatal_status(status);
    }
}

void nested_noreturn_guard(void) {
    int status = annotated_status();
    if (status != 1) {
        if (status == 4) {
            fatal_status(status);
        } else {
            fatal_status(status);
        }
    }
}

void noreturn_guard_with_unreachable_tail(void) {
    int status = annotated_status();
    if (status != 1) {
        fatal_status(status);
        log_status(status);
    }
}

void ordinary_handler_does_not_exit(void) {
    if (annotated_status() != 1) {
        log_status(1);
    }
}
