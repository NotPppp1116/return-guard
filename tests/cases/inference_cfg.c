int unreachable_future_assignment(void) {
    int status = 0;
    return status;
    status = 1;
}

void test_unreachable_future_assignment(void) {
    int status = unreachable_future_assignment();
    if (status == 0) {
        return;
    }
}

int escaped_local_value(int selector) {
    int status = 0;
    int* alias = &status;
    if (selector) {
        *alias = 1;
    }
    return status;
}

void test_escaped_local_value(int selector) {
    int status = escaped_local_value(selector);
    if (status == 0 || status == 1) {
        return;
    }
}
