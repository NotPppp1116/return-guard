#include "status_api.h"

int cpp_call_site() {
    auto status = annotated_status();
    if (status == 1) {
        return 0;
    }
    return 1;
}

void cpp_explicit_discard() {
    using void_t = void;
    void_t((void)annotated_status());
}
