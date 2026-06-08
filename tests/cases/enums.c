#include "status_api.h"

void incomplete_enum_switch(int selector) {
    enum status_code status = enum_status(selector);

    switch (status) {
    case STATUS_OK:
        break;
    case STATUS_RETRY:
        break;
    }
}

void complete_enum_switch(int selector) {
    switch (enum_status(selector)) {
    case STATUS_OK:
    case STATUS_RETRY:
    case STATUS_DENIED:
        break;
    }
}
