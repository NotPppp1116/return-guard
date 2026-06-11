#pragma once
#pragma clang system_header

extern "C" {

typedef unsigned int rg_socklen_t;
typedef unsigned long rg_nfds_t;

struct pollfd;
struct timespec;
struct sigset_t;
struct itimerspec;
struct stat;

void* fmemopen(void* buffer, unsigned long size, const char* mode);
void* open_memstream(char** pointer, unsigned long* size);
void* fdopendir(int fd);
void* dlopen(const char* filename, int flags);

int remove(const char* path);
int rename(const char* old_path, const char* new_path);
int socketpair(int domain, int type, int protocol, int pair[2]);
int getsockname(int fd, void* address, rg_socklen_t* length);
int setsockopt(int fd, int level, int option, const void* value, rg_socklen_t length);
int pipe2(int pipefd[2], int flags);
int eventfd(unsigned int initial_value, int flags);
int inotify_init1(int flags);
int epoll_ctl(int epoll_fd, int operation, int fd, void* event);
int timerfd_settime(int fd, int flags, const struct itimerspec* new_value, struct itimerspec* old_value);
int chdir(const char* path);
int mkdir(const char* path, unsigned int mode);
int unlinkat(int fd, const char* path, int flags);
int symlinkat(const char* target, int newdirfd, const char* linkpath);
int readlink(const char* path, char* buffer, unsigned long size);
int fchmodat(int fd, const char* path, unsigned int mode, int flags);
int fchownat(int fd, const char* path, unsigned int owner, unsigned int group, int flags);
int faccessat(int fd, const char* path, int mode, int flags);
int fstatat(int fd, const char* path, struct stat* status, int flags);
int ppoll(struct pollfd* fds, rg_nfds_t count, const struct timespec* timeout, const struct sigset_t* mask);
int pselect(int nfds, void* readfds, void* writefds, void* exceptfds, const struct timespec* timeout, const struct sigset_t* mask);

void* wl_display_create(void);
void* wl_event_loop_add_fd(void* loop, int fd, unsigned int mask, void* callback, void* data);
int wl_display_flush(void* display);
int wl_event_source_remove(void* source);
int wl_proxy_add_listener(void* proxy, void (**implementation)(void), void* data);

} // extern "C"

namespace vendor {

inline int chdir(const char*) {
    return -1;
}

inline int remove(const char*) {
    return -1;
}

inline void* wl_display_create(void) {
    return nullptr;
}

} // namespace vendor
