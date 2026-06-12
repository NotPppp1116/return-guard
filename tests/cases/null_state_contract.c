#include <returnguard/Contracts.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct item {
    int value;
};

struct item* annotated_failure(void) RETURNGUARD_FAILS_NULL;
struct item* configured_failure(void);

int unchecked_annotated_failure_contract(void) {
    return annotated_failure()->value;
}

int unchecked_configured_failure_contract(void) {
    return configured_failure()->value;
}

int checked_configured_failure_contract(void) {
    struct item* item = configured_failure();
    if (item == 0) {
        return 0;
    }
    return item->value;
}

int unchecked_strchr_contract(const char* text) {
    return strchr(text, '=')[0];
}

int checked_strchr_contract(const char* text) {
    char* separator = strchr(text, '=');
    if (separator == 0) {
        return 0;
    }
    return separator[0];
}

int unchecked_strstr_contract(const char* text) {
    return strstr(text, "needle")[0];
}

int checked_strstr_contract(const char* text) {
    char* needle = strstr(text, "needle");
    if (needle == 0) {
        return 0;
    }
    return needle[0];
}

int unchecked_memchr_contract(const char* text, unsigned long size) {
    return ((char*)memchr(text, '\n', size))[0];
}

int checked_memchr_contract(const char* text, unsigned long size) {
    char* newline = (char*)memchr(text, '\n', size);
    if (newline == 0) {
        return 0;
    }
    return newline[0];
}

int unchecked_fgets_contract(char* buffer, int size, FILE* file) {
    return fgets(buffer, size, file)[0];
}

int checked_fgets_contract(char* buffer, int size, FILE* file) {
    char* line = fgets(buffer, size, file);
    if (line == 0) {
        return 0;
    }
    return line[0];
}

int unchecked_getcwd_contract(char* buffer, unsigned long size) {
    return getcwd(buffer, size)[0];
}

int checked_getcwd_contract(char* buffer, unsigned long size) {
    char* path = getcwd(buffer, size);
    if (path == 0) {
        return 0;
    }
    return path[0];
}
