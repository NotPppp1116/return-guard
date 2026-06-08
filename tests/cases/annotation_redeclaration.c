#include "status_api.h"

RG_VALUES(+1, 4, 32)
static int redeclared_status(void);

static int redeclared_status(void) {
    return open_status(0);
}

void incomplete_redeclared_annotation(void) {
    int status = redeclared_status();
    if (status == 1) {
        return;
    }
}
