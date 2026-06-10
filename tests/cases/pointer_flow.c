#include "status_api.h"

int checked_through_pointer(void) {
    int status = annotated_status();
    int* pointer = &status;

    if (*pointer == 1 || *pointer == 4 || *pointer == 32) {
        return 0;
    }
    return 1;
}

int stored_through_pointer(void) {
    int status;
    int* pointer = &status;
    *pointer = annotated_status();

    switch (status) {
    case 1:
    case 4:
    case 32:
        return 0;
    }
    return 1;
}

int checked_through_pointer_copy(void) {
    int status = annotated_status();
    int* first = &status;
    int* second = first;

    if (*second != 1 && *second != 4 && *second != 32) {
        return 1;
    }
    return 0;
}

int partially_checked_through_pointer(void) {
    int status = annotated_status();
    int* pointer = &status;

    if (*pointer == 1) {
        return 0;
    }
    return 1;
}
