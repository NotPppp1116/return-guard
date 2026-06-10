#include <cstdlib>

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
