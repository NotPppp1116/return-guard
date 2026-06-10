#define RG_NULLABLE __attribute__((annotate("returnguard.nullable")))

struct item {
    int value;
};

RG_NULLABLE struct item* nullable_item(void);

int strict_unchecked_pointer(void) {
    return nullable_item()->value;
}
