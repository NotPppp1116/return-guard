static int finite_status(int selector) {
    if (selector == 0) {
        return 1;
    }
    if (selector == 1) {
        return 4;
    }
    return 32;
}

void incomplete_integer_handling(int selector) {
    int status = finite_status(selector);
    if (status == 1) {
        return;
    } else if (status == 4) {
        return;
    }
}

void complete_integer_handling(int selector) {
    int status = finite_status(selector);
    if (status == 1) {
        return;
    } else if (status == 4) {
        return;
    } else {
        return;
    }
}
