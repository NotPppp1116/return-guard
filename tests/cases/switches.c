#include "status_api.h"

void open_domain_without_default(int selector) {
    switch (open_status(selector)) {
    case 0:
        break;
    case 1:
        break;
    }
}

void open_domain_with_default(int selector) {
    switch (open_status(selector)) {
    case 0:
        break;
    default:
        break;
    }
}
