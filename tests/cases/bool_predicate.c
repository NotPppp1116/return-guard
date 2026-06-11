int work(void);
_Bool ready(void);

int practical_boolean_condition(void) {
    if (ready()) {
        return work();
    }
    return 0;
}
