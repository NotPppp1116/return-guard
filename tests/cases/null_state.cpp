#include <returnguard/Nullability.h>
#include <returnguard/Contracts.h>

struct node {
    int value;
};

node* RETURNGUARD_NULLABLE nullable_node();

struct pointer_like {
    node* operator->() RETURNGUARD_FAILS_NULL;
};

template <class T>
struct owner_box {
    T* get() RETURNGUARD_FAILS_NULL;
};

template <class T>
owner_box<T> makeUnique();

struct owner_vec {
    owner_box<node>& emplace_back(owner_box<node>);
};

extern owner_vec g_owner_vec;

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

int nonnull_factory_get_is_clean() {
    node* result = makeUnique<node>().get();
    return result->value;
}

int nonnull_emplace_back_get_is_clean() {
    node* result = g_owner_vec.emplace_back(makeUnique<node>()).get();
    return result->value;
}
