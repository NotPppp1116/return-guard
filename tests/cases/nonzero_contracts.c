#define _GNU_SOURCE

#include <grp.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stddef.h>

__attribute__((annotate("returnguard.failure:nonzero"))) int annotated_nonzero_status(void);

int configured_nonzero_status(void);

int unsafe_pthread_create_contract(pthread_t* thread, void* (*start)(void*), void* argument) {
    int status = pthread_create(thread, 0, start, argument);
    return status + 1;
}

int checked_pthread_create_contract(pthread_t* thread, void* (*start)(void*), void* argument) {
    int status = pthread_create(thread, 0, start, argument);
    if (status != 0) {
        return status;
    }
    return 0;
}

int checked_bare_pthread_create_contract(pthread_t* thread, void* (*start)(void*),
                                         void* argument) {
    int status = pthread_create(thread, 0, start, argument);
    if (status) {
        return status;
    }
    return 0;
}

int weak_negative_pthread_create_contract(pthread_t* thread, void* (*start)(void*),
                                          void* argument) {
    int status = pthread_create(thread, 0, start, argument);
    if (status < 0) {
        return -1;
    }
    return status + 1;
}

int unsafe_mutex_lock_contract(pthread_mutex_t* mutex) {
    int status = pthread_mutex_lock(mutex);
    return status + 1;
}

int checked_direct_mutex_lock_contract(pthread_mutex_t* mutex) {
    if (pthread_mutex_lock(mutex) != 0) {
        return -1;
    }
    return 0;
}

int checked_bare_direct_mutex_unlock_contract(pthread_mutex_t* mutex) {
    if (pthread_mutex_unlock(mutex)) {
        return -1;
    }
    return 0;
}

int unsafe_getpwnam_r_contract(char* buffer, size_t size) {
    struct passwd password;
    struct passwd* result = 0;
    int status = getpwnam_r("root", &password, buffer, size, &result);
    return status + 1;
}

int checked_getpwnam_r_contract(char* buffer, size_t size) {
    struct passwd password;
    struct passwd* result = 0;
    int status = getpwnam_r("root", &password, buffer, size, &result);
    if (status != 0) {
        return status;
    }
    return result != 0;
}

int unsafe_getgrgid_r_contract(char* buffer, size_t size) {
    struct group group;
    struct group* result = 0;
    int status = getgrgid_r(0, &group, buffer, size, &result);
    return status + 1;
}

int checked_getgrgid_r_contract(char* buffer, size_t size) {
    struct group group;
    struct group* result = 0;
    int status = getgrgid_r(0, &group, buffer, size, &result);
    if (status) {
        return status;
    }
    return result != 0;
}

int unsafe_raise_contract(int signal_number) {
    int status = raise(signal_number);
    return status + 1;
}

int checked_raise_contract(int signal_number) {
    int status = raise(signal_number);
    if (status != 0) {
        return -1;
    }
    return status;
}

int unsafe_annotated_nonzero_contract(void) {
    int status = annotated_nonzero_status();
    return status + 1;
}

int checked_annotated_nonzero_contract(void) {
    int status = annotated_nonzero_status();
    if (status) {
        return status;
    }
    return 0;
}

int unsafe_configured_nonzero_contract(void) {
    int status = configured_nonzero_status();
    return status + 1;
}

int checked_configured_nonzero_contract(void) {
    int status = configured_nonzero_status();
    if (status != 0) {
        return status;
    }
    return 0;
}
