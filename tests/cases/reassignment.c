#include "status_api.h"

void overwritten_before_check(void) {
    int status = annotated_status();
    status = 1;

    switch (status) {
    default:
        break;
    }
}
