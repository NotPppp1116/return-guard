#include <returnguard/Contracts.h>

struct Packet {
    int value;
};

static struct Packet packet = {.value = 41};
static int call_count;

struct Packet* make_packet(void) RETURNGUARD_FAILS_NULL;
int positive_status(void) RETURNGUARD_FAILS_NEGATIVE;

struct Packet* make_packet(void) {
    ++call_count;
    return &packet;
}

int positive_status(void) {
    ++call_count;
    return 7;
}

int main(void) {
    call_count = 0;
    const int value = make_packet()->value;
    if (call_count != 1 || value != 41) {
        return 1;
    }

    call_count = 0;
    const int status = positive_status() + 1;
    if (call_count != 1 || status != 8) {
        return 2;
    }

    return 0;
}
