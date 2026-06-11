#pragma once

static int* header_stack_escape(void) {
    int value = 0;
    return &value;
}
