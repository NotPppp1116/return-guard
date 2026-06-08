#include "status_api.h"

void comma_rhs_initializes_result(void) {
    int status = ((void)open_status(0), annotated_status());

    if (status == 1) {
        return;
    } else if (status == 4) {
        return;
    }
}

int comma_rhs_is_forwarded(void) {
    return ((void)open_status(0), annotated_status());
}
