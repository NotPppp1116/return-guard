#include "status_api.h"

void compound_assignment_consumes_result(void) {
    int status = 0;
    status += annotated_status();

    switch (status) {
    default:
        break;
    }
}
