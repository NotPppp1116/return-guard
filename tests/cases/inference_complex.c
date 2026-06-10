#include "status_api.h"

// Large complex function with many branches and variable reassignments
int deep_inference_mega(int a, int b, int c) {
    int result = 0;
    
    if (a > 10) {
        int x = 1;
        if (b < 0) {
            x = -1;
        } else if (b > 100) {
            x = -2;
        }
        result = x;
    } else {
        int y = 2;
        switch (c) {
            case 0: y = 10; break;
            case 1: y = 20; break;
            case 2: y = 30; break;
            default: y = 40; break;
        }
        result = y;
    }
    
    // Some dead code or noise to increase size
    if (result == 999) {
        result = 0;
    }
    
    return result;
}

// Another complex one with nested ternary and multiple variables
int nested_ternary_inference(int cond1, int cond2, int cond3) {
    int v1 = cond1 ? 100 : 200;
    int v2 = cond2 ? (cond3 ? 1 : 2) : (cond3 ? 3 : 4);
    int final_v = 0;
    
    if (v1 == 100) {
        final_v = v2; // 1, 2
    } else {
        final_v = v2 + 10; // 13, 14
    }
    
    return final_v;
}

// Function that uses previous ones to test chain inference at scale
int tiered_proxy_complex(int x, int y, int z) {
    int s1 = deep_inference_mega(x, y, z);
    int res = 0;
    
    if (s1 == -1 || s1 == -2) {
        res = 500;
    } else if (s1 == 1) {
        res = 600;
    } else {
        int s2 = nested_ternary_inference(x, y, z);
        if (s2 < 10) {
            res = 700;
        } else {
            res = 800;
        }
    }
    
    return res;
}

void test_mega_exhaustive(int a, int b, int c) {
    int s = deep_inference_mega(a, b, c);
    // Possible values: -1, -2, 1 (from a > 10 path)
    // 10, 20, 30, 40 (from else path)
    // AND 0 (initializer), 2 (y initializer)
    if (s == -1 || s == -2 || s == 1 || s == 10 || s == 20 || s == 30 || s == 40 || s == 0 || s == 2) {
        return;
    }
} // Should NOT warn

void test_mega_missing(int a, int b, int c) {
    int s = deep_inference_mega(a, b, c);
    if (s == -1 || s == -2 || s == 1 || s == 10 || s == 20 || s == 30 || s == 0 || s == 2) {
        return;
    }
} // Should warn: missing 40

void test_ternary_exhaustive(int a, int b, int c) {
    int s = nested_ternary_inference(a, b, c);
    // v1=100 -> v2={1,2} -> final_v={1,2}
    // v1=200 -> v2={3,4} -> final_v={13,14}
    if (s == 1 || s == 2 || s == 13 || s == 14) {
        return;
    }
} // Should NOT warn

void test_ternary_missing(int a, int b, int c) {
    int s = nested_ternary_inference(a, b, c);
    if (s == 1 || s == 2 || s == 13) {
        return;
    }
} // Should warn: missing 14

void test_tiered_exhaustive(int a, int b, int c) {
    int s = tiered_proxy_complex(a, b, c);
    // res can be 500, 600, 700, 800, 0
    if (s == 500 || s == 600 || s == 700 || s == 800 || s == 0) {
        return;
    }
} // Should NOT warn

void test_tiered_missing(int a, int b, int c) {
    int s = tiered_proxy_complex(a, b, c);
    if (s == 500 || s == 600 || s == 700 || s == 0) {
        return;
    }
} // Should warn: missing 800

// Stressing recursive variable tracking
int recursion_stress(int x) {
    int a = 1;
    int b = a;
    int c = b;
    int d = c;
    int e = d;
    if (x) {
        e = 2;
    }
    return e;
}

void test_recursion_stress(int x) {
    int s = recursion_stress(x);
    if (s == 1 || s == 2) return;
} // Should NOT warn

// Complex conditional with many return points
int fragmented_returns(int x, int y) {
    if (x > 0) {
        if (y > 0) return 100;
        if (y < 0) return 101;
        return 102;
    }
    if (x < 0) {
        int res = 200;
        if (y == 0) res = 201;
        return res;
    }
    return 300;
}

void test_fragmented_exhaustive(int x, int y) {
    int s = fragmented_returns(x, y);
    if (s == 100 || s == 101 || s == 102 || s == 200 || s == 201 || s == 300) return;
} // Should NOT warn

int get_1(void) { return 1; }
int get_2(void) { return 2; }
int arithmetic_inference(void) {
    return get_1() + get_2();
}

void test_arithmetic_inference(void) {
    int s = arithmetic_inference();
    if (s == 3) return;
} // Should NOT warn
