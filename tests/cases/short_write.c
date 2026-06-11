#define _XOPEN_SOURCE 700

#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

int direct_write_negative_only(int fd, const char* buffer, size_t size) {
    if (write(fd, buffer, size) < 0) {
        return -1;
    }
    return 0;
}

int direct_pwrite_negative_only(int fd, const char* buffer, size_t size) {
    if (pwrite(fd, buffer, size, 0) == -1) {
        return -1;
    }
    return 0;
}

int direct_send_negative_only(int fd, const char* buffer, size_t size) {
    if (send(fd, buffer, size, 0) < 0) {
        return -1;
    }
    return 0;
}

int direct_sendto_negative_only(int fd, const char* buffer, size_t size) {
    if (sendto(fd, buffer, size, 0, (const struct sockaddr*)0, (socklen_t)0) < 0) {
        return -1;
    }
    return 0;
}

int direct_read_negative_only(int fd, char* buffer, size_t size) {
    if (read(fd, buffer, size) < 0) {
        return -1;
    }
    return 0;
}

int direct_pread_negative_only(int fd, char* buffer, size_t size) {
    if (pread(fd, buffer, size, 0) == -1) {
        return -1;
    }
    return 0;
}

int direct_recv_negative_only(int fd, char* buffer, size_t size) {
    if (recv(fd, buffer, size, 0) < 0) {
        return -1;
    }
    return 0;
}

int direct_recvfrom_negative_only(int fd, char* buffer, size_t size) {
    if (recvfrom(fd, buffer, size, 0, (struct sockaddr*)0, (socklen_t*)0) < 0) {
        return -1;
    }
    return 0;
}

int assigned_write_negative_only(int fd, const char* buffer, size_t size) {
    ssize_t written = write(fd, buffer, size);
    if (written < 0) {
        return -1;
    }
    return 0;
}

int assignment_condition_write_negative_only(int fd, const char* buffer, size_t size) {
    ssize_t written;
    if ((written = write(fd, buffer, size)) < 0) {
        return -1;
    }
    return (int)written;
}

int conditional_write_negative_only(int fd, const char* buffer, size_t size) {
    return write(fd, buffer, size) < 0 ? -1 : 0;
}

int conditional_assignment_write_negative_only(int fd, const char* buffer, size_t size) {
    ssize_t written;
    return (written = write(fd, buffer, size)) < 0 ? -1 : (int)written;
}

int count_checked_write(int fd, const char* buffer, size_t size) {
    if (write(fd, buffer, size) != (ssize_t)size) {
        return -1;
    }
    return 0;
}

int count_checked_read(int fd, char* buffer, size_t size) {
    if (read(fd, buffer, size) != (ssize_t)size) {
        return -1;
    }
    return 0;
}

int count_checked_assignment(int fd, const char* buffer, size_t size) {
    ssize_t written = write(fd, buffer, size);
    if (written != (ssize_t)size) {
        return -1;
    }
    return 0;
}

int count_checked_assignment_condition(int fd, const char* buffer, size_t size) {
    ssize_t written;
    if ((written = write(fd, buffer, size)) != (ssize_t)size) {
        return -1;
    }
    return 0;
}

int count_checked_conditional(int fd, const char* buffer, size_t size) {
    return write(fd, buffer, size) == (ssize_t)size ? 0 : -1;
}

int count_checked_assignment_conditional(int fd, const char* buffer, size_t size) {
    ssize_t written;
    return (written = write(fd, buffer, size)) == (ssize_t)size ? 0 : -1;
}
