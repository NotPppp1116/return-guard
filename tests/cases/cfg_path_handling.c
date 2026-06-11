#include "status_api.h"

int branch_checked_but_other_path_uses_unchecked(int flag) {
    int status = annotated_status();

    if (flag) {
        if (status == 1 || status == 4 || status == 32) {
            return 0;
        }
        return 1;
    }

    return status + 1;
}

int all_paths_checked_before_use(int flag) {
    int status = annotated_status();

    if (flag) {
        if (status == 1 || status == 4 || status == 32) {
            return 0;
        }
        return 1;
    }

    switch (status) {
    case 1:
    case 4:
    case 32:
        return 0;
    }
    return 1;
}
