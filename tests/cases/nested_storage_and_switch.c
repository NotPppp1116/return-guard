#include "status_api.h"

static int normalize_status(int status) {
    return status;
}

void nested_initializer(void) {
    int status = normalize_status(annotated_status());
    if (status) {
        return;
    } else {
        return;
    }
}

void nested_switch(void) {
    switch (normalize_status(annotated_status())) {
    default:
        break;
    }
}
