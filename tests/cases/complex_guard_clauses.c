#include "status_api.h"

void direct_negated_guard_clause(void) {
    if (!(annotated_status() == 1)) {
        return;
    }
}

void stored_negated_guard_clause(void) {
    int status = annotated_status();
    if (!(status == 1)) {
        return;
    }
}

void stored_allowed_set_guard_clause(void) {
    int status = annotated_status();
    if (status != 1 && status != 4) {
        return;
    }
}

void stored_negated_allowed_set_guard_clause(void) {
    int status = annotated_status();
    if (!(status == 1 || status == 4)) {
        return;
    }
}

void stored_range_or_guard_clause(int selector) {
    int status = open_status(selector);
    if (status < 0 || status > 255) {
        return;
    }
}

void negated_error_check_is_not_guard(void) {
    int status = annotated_status();
    if (!(status != 1)) {
        return;
    }
}
