#include "status_api.h"

void incomplete_if_chain(void) {
    int status = annotated_status();
    if (status == 1 || status == 4) {
        return;
    }
}

void complete_if_chain(void) {
    int status = annotated_status();
    if (status == 1) {
        return;
    } else if (status == 4) {
        return;
    } else if (status == 32) {
        return;
    }
}
