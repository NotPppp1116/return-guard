#define _GNU_SOURCE

#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdlib.h>
#include <dirent.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/types.h>
#include <sys/wait.h>
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

int unsafe_fileno_contract(FILE* file) {
    int fd = fileno(file);
    return fd + 1;
}

int checked_fileno_contract(FILE* file) {
    int fd = fileno(file);
    if (fd < 0) {
        return -1;
    }
    return fd;
}

int unsafe_dirfd_contract(DIR* directory) {
    int fd = dirfd(directory);
    return fd + 1;
}

int checked_dirfd_contract(DIR* directory) {
    int fd = dirfd(directory);
    if (fd < 0) {
        return -1;
    }
    return fd;
}

int unsafe_ftell_contract(FILE* file) {
    long offset = ftell(file);
    return (int)(offset + 1);
}

int checked_ftell_contract(FILE* file) {
    long offset = ftell(file);
    if (offset < 0) {
        return -1;
    }
    return (int)offset;
}

int unsafe_mkstemp_contract(char* path) {
    int fd = mkstemp(path);
    return fd + 1;
}

int checked_mkstemp_contract(char* path) {
    int fd = mkstemp(path);
    if (fd < 0) {
        return -1;
    }
    return fd;
}

int unsafe_system_contract(const char* command) {
    int status = system(command);
    return status + 1;
}

int checked_system_contract(const char* command) {
    int status = system(command);
    if (status < 0) {
        return -1;
    }
    return status;
}

int unsafe_setuid_contract(void) {
    int status = setuid(0);
    return status + 1;
}

int checked_setuid_contract(void) {
    int status = setuid(0);
    if (status < 0) {
        return -1;
    }
    return status;
}

int unsafe_chroot_contract(void) {
    int status = chroot("/");
    return status + 1;
}

int checked_chroot_contract(void) {
    int status = chroot("/");
    if (status < 0) {
        return -1;
    }
    return status;
}

int unsafe_prctl_contract(void) {
    int status = prctl(0, 0, 0, 0, 0);
    return status + 1;
}

int unsafe_mprotect_contract(void* address, size_t size) {
    int status = mprotect(address, size, PROT_READ);
    return status + 1;
}

int checked_mprotect_contract(void* address, size_t size) {
    int status = mprotect(address, size, PROT_READ);
    if (status < 0) {
        return -1;
    }
    return status;
}

int unsafe_mlockall_contract(void) {
    int status = mlockall(0);
    return status + 1;
}

int unsafe_shm_open_contract(void) {
    int fd = shm_open("/returnguard-contract", O_RDONLY, 0);
    return fd + 1;
}

int checked_shm_open_contract(void) {
    int fd = shm_open("/returnguard-contract", O_RDONLY, 0);
    if (fd < 0) {
        return -1;
    }
    return fd;
}

int unsafe_fork_contract(void) {
    pid_t child = fork();
    return (int)(child + 1);
}

int checked_fork_contract(void) {
    pid_t child = fork();
    if (child < 0) {
        return -1;
    }
    return (int)child;
}

int unsafe_execve_contract(const char* path, char* const* argv, char* const* envp) {
    int status = execve(path, argv, envp);
    return status + 1;
}

int unsafe_waitpid_contract(pid_t child) {
    int status_value = 0;
    pid_t result = waitpid(child, &status_value, 0);
    return (int)(result + 1);
}

int checked_waitpid_contract(pid_t child) {
    int status_value = 0;
    pid_t result = waitpid(child, &status_value, 0);
    if (result < 0) {
        return -1;
    }
    return (int)result;
}

int unsafe_getentropy_contract(void* buffer, size_t size) {
    int status = getentropy(buffer, size);
    return status + 1;
}

int checked_getentropy_contract(void* buffer, size_t size) {
    int status = getentropy(buffer, size);
    if (status < 0) {
        return -1;
    }
    return status;
}

int unsafe_kill_contract(pid_t process) {
    int status = kill(process, 0);
    return status + 1;
}

int checked_kill_contract(pid_t process) {
    int status = kill(process, 0);
    if (status < 0) {
        return -1;
    }
    return status;
}

int unsafe_sigaction_contract(int signal_number, const struct sigaction* action) {
    int status = sigaction(signal_number, action, 0);
    return status + 1;
}

int unsafe_ioctl_contract(int fd) {
    int status = ioctl(fd, 0);
    return status + 1;
}

int checked_ioctl_contract(int fd) {
    int status = ioctl(fd, 0);
    if (status < 0) {
        return -1;
    }
    return status;
}

int unsafe_fcntl_contract(int fd) {
    int status = fcntl(fd, F_GETFD);
    return status + 1;
}

int checked_fcntl_contract(int fd) {
    int status = fcntl(fd, F_GETFD);
    if (status < 0) {
        return -1;
    }
    return status;
}

int unsafe_flock_contract(int fd) {
    int status = flock(fd, LOCK_EX);
    return status + 1;
}
