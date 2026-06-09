#include "status_api.h"

void direct_guard_clause(void) {
    if (annotated_status() != 1) {
        return;
    }
}

void stored_guard_clause(void) {
    int status = annotated_status();
    if (status != 1) {
        return;
    }
}

void stored_range_guard_clause(int selector) {
    int status = open_status(selector);
    if (status < 0) {
        return;
    }
}

void equality_without_else_is_not_guard(void) {
    if (annotated_status() == 1) {
        return;
    }
}
