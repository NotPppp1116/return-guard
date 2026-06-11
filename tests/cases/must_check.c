int must_check_status(void) __attribute__((warn_unused_result));

int consumed_must_check(void) {
    int status = must_check_status();
    return status + 1;
}

int checked_must_check(void) {
    int status = must_check_status();
    if (status < 0) {
        return status;
    }
    return 0;
}

int ignored_must_check(void) {
    must_check_status();
    return 0;
}
