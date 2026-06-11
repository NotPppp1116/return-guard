#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>

#include <returnguard/Contracts.h>

void* malloc(size_t size) RETURNGUARD_FAILS_NULL;

int consume_fd(int fd);

int instrumentation_sample(const char* path, size_t size) {
    void* memory = malloc(size);

    int handled = open(path, O_RDONLY);
    if (handled < 0) {
        free(memory);
        return -1;
    }

    int nested = consume_fd(open(path, O_RDONLY));
    close(handled);
    free(memory);
    return nested;
}
