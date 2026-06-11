#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

#include "include/vendor_system_api.hpp"

static int consume_status(int value) {
    return value;
}

static void consume_file(std::FILE* file) {
    (void)file;
}

struct Node {
    int value;
};

Node* null_factory();

void system_namespace_sample(void) {
    consume_status(vendor::open("ignored", 0));
    consume_status(::openat(AT_FDCWD, "ignored", O_RDONLY));
    consume_status(::close(-1));
    consume_status(std::fputs("ignored", stderr));
    consume_file(std::fopen("ignored", "r"));
}

int configured_byte_count_contract_sample(int fd, const char* buffer, unsigned long size) {
    if (vendor::send_like(fd, buffer, size) < 0) {
        return -1;
    }
    return 0;
}

int configured_null_contract_sample(void) {
    return null_factory()->value;
}

int system_malloc_contract_sample(void) {
    Node* node = static_cast<Node*>(std::malloc(sizeof(Node)));
    return node->value;
}

int system_getenv_contract_sample(void) {
    return ::getenv("RETURNGUARD_CONTRACT_SAMPLE")[0];
}
