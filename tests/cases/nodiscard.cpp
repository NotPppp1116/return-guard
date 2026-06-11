[[nodiscard]] int nodiscard_status();

int consumed_nodiscard() {
    int status = nodiscard_status();
    return status + 1;
}

int checked_nodiscard() {
    int status = nodiscard_status();
    if (status < 0) {
        return status;
    }
    return 0;
}
