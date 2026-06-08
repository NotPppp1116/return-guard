#include "status_api.h"

void ignored(void) {
    annotated_status();
}

void explicitly_ignored(void) {
    (void)annotated_status();
}
