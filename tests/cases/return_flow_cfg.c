#include "status_api.h"

static void exhaustive_status_helper(int status) {
    if (status == 1 || status == 4 || status == 32) {
        return;
    }
}

static void unreachable_status_helper(int status) {
    return;
    if (status == 1 || status == 4 || status == 32) {
        return;
    }
}

void checked_after_join(int selector) {
    int status = annotated_status();
    int alias;
    if (selector) {
        alias = status;
    } else {
        alias = status;
    }
    if (alias == 1 || alias == 4 || alias == 32) {
        return;
    }
}

void checked_by_helper(void) {
    int status = annotated_status();
    exhaustive_status_helper(status);
}

struct status_record {
    int status;
};

void checked_through_member(void) {
    struct status_record record;
    record.status = annotated_status();
    if (record.status == 1 || record.status == 4 || record.status == 32) {
        return;
    }
}

void helper_with_unreachable_check_does_not_count(void) {
    int status = annotated_status();
    unreachable_status_helper(status);
}

void unreachable_check_does_not_count(void) {
    int status = annotated_status();
    return;
    if (status == 1 || status == 4 || status == 32) {
        return;
    }
}

void reassigned_alias_invalidates(int selector) {
    int status = annotated_status();
    int alias = status;
    if (selector) {
        alias = 1;
    }
    if (alias == 1 || alias == 4 || alias == 32) {
        return;
    }
}
