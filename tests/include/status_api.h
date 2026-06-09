#ifndef RETURNGUARD_TEST_STATUS_API_H
#define RETURNGUARD_TEST_STATUS_API_H

#if defined(__clang__) || defined(__GNUC__)
#define RG_VALUES(...) \
    __attribute__((annotate("returnguard.values:" #__VA_ARGS__)))
#define RG_NORETURN __attribute__((noreturn))
#else
#define RG_VALUES(...)
#define RG_NORETURN
#endif

enum status_code {
    STATUS_OK = 0,
    STATUS_RETRY = 4,
    STATUS_DENIED = 32,
};

enum status_code enum_status(int selector);
RG_VALUES(1, 4, 32) int annotated_status(void);
int open_status(int selector);
RG_NORETURN void fatal_status(int status);
void log_status(int status);

#endif
