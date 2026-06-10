#include "status_api.h"

int inferred_direct(int x) {
    if (x) return 1;
    return 0;
}

int inferred_variable(int x) {
    int res = 0;
    if (x) res = 1;
    return res;
}

void test_inferred_direct(int x) {
    int s = inferred_direct(x);
    if (s == 0 || s == 1) { // Should be exhaustive
        return;
    }
} // Should NOT warn

void test_inferred_variable(int x) {
    int s = inferred_variable(x);
    if (s == 0 || s == 1) { // Should be exhaustive if inferred
        return;
    }
} // Should NOT warn

int not_inferred(int x) {
    int res = x;
    return res;
}

void test_not_inferred(int x) {
    int s = not_inferred(x);
    if (s == 0 || s == 1) { // Should warn in strict mode
        return;
    }
}

int inferred_complex_fail(int x) {
    int res = 0;
    res += x;
    return res;
}

void test_inferred_complex_fail(int x) {
    int s = inferred_complex_fail(x);
    if (s == 0) return;
}

int get_status(void) { return -1; }
int proxy_status(void) {
    int s = get_status();
    return s;
}

void test_proxy(void) {
    int s = proxy_status();
    if (s == -1) return;
} // Should NOT warn
