int recursive_value_b(int value);

int recursive_value_a(int value) {
    return recursive_value_b(value);
}

int recursive_value_b(int value) {
    return recursive_value_a(value);
}

int value_inference_recursion_sample(void) {
    int rc = recursive_value_a(0);
    if (rc == 0) {
        return 0;
    }
    return 1;
}
