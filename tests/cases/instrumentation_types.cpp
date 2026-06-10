#include <returnguard/Contracts.h>

struct Widget {
    int value;
};

static Widget widget{73};
static int call_count;

Widget* make_widget() RETURNGUARD_FAILS_NULL;
int positive_status_cpp() RETURNGUARD_FAILS_NEGATIVE;

Widget* make_widget() {
    ++call_count;
    return &widget;
}

int positive_status_cpp() {
    ++call_count;
    return 5;
}

int main() {
    call_count = 0;
    const int value = make_widget()->value;
    if (call_count != 1 || value != 73) {
        return 1;
    }

    call_count = 0;
    const int status = positive_status_cpp() + 2;
    if (call_count != 1 || status != 7) {
        return 2;
    }

    return 0;
}
