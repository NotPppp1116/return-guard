#include <cstdio>

#include "include/vendor_system_api.hpp"

static int consume_status(int value) {
    return value;
}

static void consume_file(std::FILE* file) {
    (void)file;
}

void system_namespace_sample(void) {
    consume_status(vendor::open("ignored", 0));
    consume_file(std::fopen("ignored", "r"));
}
