#include "unsafe_header.h"

int* uses_header_escape(void) {
    return header_stack_escape();
}
