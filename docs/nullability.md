# Nullable pointer return analysis

ReturnGuard can mark pointer-returning APIs as nullable and verify that their results are guarded before dereference.

## Marking a return nullable

With Clang nullability syntax:

```c
#include <returnguard/Nullability.h>

struct item;
struct item* RETURNGUARD_NULLABLE find_item(int id);
```

The macro expands to Clang's `_Nullable`. ReturnGuard also recognizes the native spelling directly:

```c
struct item* _Nullable find_item(int id);
```

For generated declarations or projects that cannot use type nullability syntax, the analyzer also recognizes:

```c
__attribute__((annotate("returnguard.nullable")))
struct item* find_item(int id);
```

Annotations are read across the function's redeclaration chain, so they may live in a public header.

## Recognized guards

ReturnGuard follows the result through local pointer assignments and aliases using Clang's CFG. These patterns are accepted:

```c
struct item* item = find_item(7);
if (item == NULL) {
    return -1;
}
return item->value;
```

```c
struct item* item = find_item(7);
if (item) {
    return item->value;
}
return -1;
```

```c
return item != NULL && item->ready;
```

A guard through an alias also refines the original pointer:

```c
struct item* item = find_item(7);
struct item* alias = item;
if (!alias) {
    return -1;
}
return item->value;
```

The analysis diagnoses unsafe unary dereferences, `->`, array subscripting, and calls through nullable function pointers. A check performed only after the dereference does not count.

## Control-flow behavior

Null states are path-sensitive and joined conservatively. ReturnGuard understands early returns, short-circuit conditions, loops, conditional expressions, and replacement with a safe fallback:

```c
struct item fallback = {0};
struct item* item = find_item(7);
if (!item) {
    item = &fallback;
}
return item->value;
```

If one path can still carry an unchecked nullable result, the dereference is reported.

## Current boundary

The analysis is intraprocedural. Passing the pointer to another function does not prove that the callee checked it. Storage through aggregates, arbitrary pointer arithmetic, callbacks that mutate aliases, and opaque external effects are handled conservatively.
