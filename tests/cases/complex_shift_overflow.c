#include <stdint.h>

int shift_overflow_signed(void) {
    int x = 1 << 31;
    return x;
}

int shift_no_overflow(void) {
    unsigned int y = 1u << 31;
    return (int)y;
}
