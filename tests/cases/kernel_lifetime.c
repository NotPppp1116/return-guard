void* kmalloc(unsigned long size, int flags);
void kfree(const void* pointer);
void kvfree(const void* pointer);

int double_kfree(void) {
    int* pointer = (int*)kmalloc(sizeof(int), 0);
    kfree(pointer);
    kfree(pointer);
    return 0;
}

int use_after_kvfree(int* pointer) {
    kvfree(pointer);
    return *pointer;
}
