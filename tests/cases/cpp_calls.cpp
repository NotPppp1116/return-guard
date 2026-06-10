#include "status_api.h"

int cpp_call_site() {
    auto status = annotated_status();
    if (status == 1) {
        return 0;
    }
    return 1;
}

