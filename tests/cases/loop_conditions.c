#include "status_api.h"

void direct_while_partial(void) {
    while (annotated_status() == 1) {
        break;
    }
}

void stored_while_partial(void) {
    int status = annotated_status();
    while (status == 1 || status == 4) {
        break;
    }
}

void stored_for_exhaustive(void) {
    int status = annotated_status();
    for (; status == 1 || status == 4 || status == 32;) {
        break;
    }
}

void stored_do_exhaustive(void) {
    int status = annotated_status();
    do {
        break;
    } while (status == 1 || status == 4 || status == 32);
}
