#include <returnguard/Nullability.h>
#include <returnguard/Contracts.h>

struct node {
    int value;
};

node* RETURNGUARD_NULLABLE nullable_node();

struct pointer_like {
    node* operator->() RETURNGUARD_FAILS_NULL;
};

int unchecked_cpp() {
    node* result = nullable_node();
    return result->value;
}

int checked_nullptr() {
    node* result = nullable_node();
    if (result == nullptr) {
        return 0;
    }
    return result->value;
}

int checked_init_statement() {
    if (node* result = nullable_node(); result != nullptr) {
        return result->value;
    }
    return 0;
}

int unsafe_after_join(bool enabled) {
    node* result = nullable_node();
    if (enabled && result) {
        return result->value;
    }
    return result->value;
}

int unchecked_direct_cpp() {
    return nullable_node()->value;
}

int operator_arrow_is_opt_in(pointer_like pointer) {
    return pointer->value;
}

node* inferred_nullable_node() {
    return nullable_node();
}

int unchecked_inferred_cpp() {
    node* result = inferred_nullable_node();
    return result->value;
}
