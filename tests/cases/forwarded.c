#include "status_api.h"

int direct_return_is_forwarded(void) {
    return annotated_status();
}

int stored_return_is_forwarded(void) {
    int status = annotated_status();
    return status;
}

int stored_cast_return_is_forwarded(void) {
    int status = annotated_status();
    return (int)status;
}

int stored_comma_return_is_forwarded(void) {
    int status = annotated_status();
    return ((void)0, status);
}

int transformed_return_is_consumed(void) {
    int status = annotated_status();
    return status + 1;
}
