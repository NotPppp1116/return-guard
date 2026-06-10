#include <cstdlib>

int* cpp_return_stack_var() {
    int local = 42;
    return &local;
}

int* cpp_return_stack_array() {
    int array[5];
    return array;
}

int* cpp_return_stack_ptr() {
    int local = 42;
    int* ptr = &local;
    return ptr;
}

int cpp_oob_access_positive() {
    int array[5] = {1, 2, 3, 4, 5};
    return array[5];
}

int cpp_oob_access_negative() {
    int array[5] = {1, 2, 3, 4, 5};
    return array[-1];
}

int cpp_uaf_delete() {
    int* ptr = new int(42);
    delete ptr;
    return *ptr;
}
