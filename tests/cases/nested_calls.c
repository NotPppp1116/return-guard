#include "status_api.h"

static void consume_status(int status) {
    (void)status;
}

static int forward_status(int status) {
    return status;
}

void result_used_as_argument(void) {
    consume_status(annotated_status());
}

int result_nested_in_return(void) {
    return forward_status(annotated_status());
}
