#include <stddef.h>
#include <unistd.h>

void contextual_io(int fd, void* buffer, size_t size) {
    (void)read(fd, buffer, size);
    (void)write(fd, buffer, size);
}
