#include <stddef.h>

#include <returnguard/Contracts.h>

struct Packet {
    int value;
};

struct Packet* unavailable_packet(void) RETURNGUARD_FAILS_NULL;

struct Packet* unavailable_packet(void) {
    return NULL;
}

static int consume_packet(struct Packet* packet) {
    return packet->value;
}

int main(void) {
    return consume_packet(unavailable_packet());
}
