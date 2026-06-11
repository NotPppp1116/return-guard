void* project_alloc(unsigned long size);
void project_free(const void* pointer);

int configured_double_free(void) {
    int* pointer = (int*)project_alloc(sizeof(int));
    project_free(pointer);
    project_free(pointer);
    return 0;
}

int configured_use_after_free(int* pointer) {
    project_free(pointer);
    return *pointer;
}
