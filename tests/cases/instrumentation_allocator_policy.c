#include <stddef.h>
#include <stdlib.h>

static void consume_pointer(void* pointer) {
    (void)pointer;
}

void allocator_policy_sample(size_t count, size_t size) {
    consume_pointer(malloc(size));
    consume_pointer(calloc(count, size));
    consume_pointer(aligned_alloc(16U, size));
    consume_pointer(realloc(NULL, size));
}
