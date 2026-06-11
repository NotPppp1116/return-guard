#include <returnguard/Contracts.h>

int overloaded_status(int value) RETURNGUARD_FAILS_NEGATIVE;
int overloaded_status(long value) RETURNGUARD_FAILS_NEGATIVE;

int overloaded_status(int value) {
    return value;
}

int overloaded_status(long value) {
    return static_cast<int>(value);
}

static int consume_status(int value) {
    return value;
}

int overload_metadata_sample(void) {
    const int first = consume_status(overloaded_status(1));
    const int second = consume_status(overloaded_status(1L));
    return first + second;
}
