#define _GNU_SOURCE

#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

int unsafe_getline_contract(FILE* file) {
    char* line = 0;
    size_t capacity = 0;
    ssize_t length = getline(&line, &capacity, file);
    return (int)(length + 1);
}

int checked_getline_contract(FILE* file) {
    char* line = 0;
    size_t capacity = 0;
    ssize_t length = getline(&line, &capacity, file);
    if (length < 0) {
        return -1;
    }
    return (int)length;
}

int unsafe_getdelim_contract(FILE* file) {
    char* line = 0;
    size_t capacity = 0;
    ssize_t length = getdelim(&line, &capacity, '\n', file);
    return (int)(length + 1);
}

int checked_getdelim_contract(FILE* file) {
    char* line = 0;
    size_t capacity = 0;
    ssize_t length = getdelim(&line, &capacity, '\n', file);
    if (length == -1) {
        return -1;
    }
    return (int)length;
}

int unsafe_lseek_contract(int fd) {
    off_t offset = lseek(fd, 0, SEEK_SET);
    return (int)(offset + 1);
}

int checked_lseek_contract(int fd) {
    off_t offset = lseek(fd, 0, SEEK_SET);
    if (offset < 0) {
        return -1;
    }
    return (int)offset;
}
