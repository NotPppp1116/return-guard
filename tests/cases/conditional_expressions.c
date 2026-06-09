#include "status_api.h"

int direct_conditional_expression(void) {
    return annotated_status() == 1 ? 10 : 20;
}

int direct_truthy_conditional_expression(void) {
    return annotated_status() ? 10 : 20;
}

int stored_conditional_expression(void) {
    int status = annotated_status();
    return status == 1 ? 10 : 20;
}

int chained_conditional_expression(void) {
    int status = annotated_status();
    return status == 1 ? 10 : status == 4 ? 20 : 30;
}

int assignment_conditional_expression(void) {
    int status;
    return (status = annotated_status()) == 1 ? 10 : 20;
}

int conditional_arm_consumes_result(int flag) {
    int status = annotated_status();
    return flag ? status : 0;
}
