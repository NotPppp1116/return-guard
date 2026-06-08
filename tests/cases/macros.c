#include "status_api.h"

#define CALL_STATUS() annotated_status()
#define HANDLE_OK(value) ((value) == 1)

void macro_generated_call(void) {
    int status = CALL_STATUS();

    if (HANDLE_OK(status)) {
        return;
    } else if (status == 4) {
        return;
    }
    /* 32 is deliberately missing. */
}
