#include "status_api.h"

void incomplete_direct_if(void) {
    if (annotated_status() == 1) {
        return;
    }
}

void complete_direct_if(void) {
    if (annotated_status() == 1) {
        return;
    } else {
        return;
    }
}
