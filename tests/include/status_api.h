#ifndef RETURNGUARD_TEST_STATUS_API_H
#define RETURNGUARD_TEST_STATUS_API_H

#if defined(__clang__) || defined(__GNUC__)
#define RG_VALUES(...) \
    __attribute__((annotate("returnguard.values:" #__VA_ARGS__)))
#else
#define RG_VALUES(...)
#endif

enum status_code {
    STATUS_OK = 0,
    STATUS_RETRY = 4,
    STATUS_DENIED = 32,
};

enum status_code enum_status(int selector);
RG_VALUES(1, 4, 32) int annotated_status(void);
int open_status(int selector);

#endif
