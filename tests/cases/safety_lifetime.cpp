#include <cstdlib>

int* external_cpp_ptr();

int delete_alias_deref() {
    int* p = new int(1);
    int* q = p;
    delete p;
    return *q;
}

int double_delete_alias() {
    int* p = new int(1);
    int* q = p;
    delete p;
    delete q;
    return 0;
}

int delete_param_uaf(int* p) {
    delete p;
    return *p;
}

int unknown_return_delete_uaf() {
    int* p = external_cpp_ptr();
    delete p;
    return *p;
}
