enum status_code {
    STATUS_OK = 0,
    STATUS_RETRY = 1,
    STATUS_FATAL = 2,
    STATUS_UNKNOWN = 3,
};

static enum status_code global_status = STATUS_UNKNOWN;

typedef enum status_code (*status_callback)(int selector);

static enum status_code leaf_status(int selector) {
    if (selector < 0) {
        return STATUS_FATAL;
    }
    if (selector == 0) {
        return STATUS_OK;
    }
    return selector == 1 ? STATUS_RETRY : STATUS_UNKNOWN;
}

static enum status_code wrapped_status(int selector) {
    return leaf_status(selector);
}

static enum status_code second_order_status(int selector) {
    if (selector == 99) {
        return STATUS_FATAL;
    }
    return wrapped_status(selector);
}

static enum status_code read_global_status(void) {
    return global_status;
}

void refresh_global_status(int selector) {
    global_status = second_order_status(selector);
}

void cross_function_chain_missing(int selector) {
    enum status_code status = second_order_status(selector);
    if (status == STATUS_OK || status == STATUS_RETRY || status == STATUS_UNKNOWN) {
        return;
    }
}

void global_backed_result_missing(void) {
    enum status_code status = read_global_status();
    if (status == STATUS_OK || status == STATUS_RETRY) {
        return;
    }
}

void callback_result_missing(status_callback callback) {
    enum status_code status = callback(1);
    if (status == STATUS_OK) {
        return;
    }
}

void cross_function_chain_exhaustive(int selector) {
    enum status_code status = second_order_status(selector);
    if (status == STATUS_OK || status == STATUS_RETRY ||
        status == STATUS_FATAL || status == STATUS_UNKNOWN) {
        return;
    }
}

void global_backed_result_exhaustive(void) {
    enum status_code status = read_global_status();
    if (status == STATUS_OK || status == STATUS_RETRY ||
        status == STATUS_FATAL || status == STATUS_UNKNOWN) {
        return;
    }
}
