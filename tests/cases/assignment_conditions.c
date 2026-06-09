#include "status_api.h"

void assignment_if_partial(void) {
    int status;
    if ((status = annotated_status()) == 1 || status == 4) {
        return;
    }
}

void assignment_if_exhaustive(void) {
    int status;
    if ((status = annotated_status()) == 1 || status == 4 || status == 32) {
        return;
    }
}

void assignment_while_partial(void) {
    int status;
    while ((status = annotated_status()) == 1) {
        break;
    }
}

void assignment_for_exhaustive(void) {
    int status;
    for (; (status = annotated_status()) == 1 || status == 4 || status == 32;) {
        break;
    }
}
