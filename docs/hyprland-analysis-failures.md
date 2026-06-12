# Hyprland analysis failures and proposed fixes

This note records two concrete analysis failures found by running ReturnGuard on
all 483 Hyprland translation units. It identifies the implementation paths that
produce the diagnostics, explains why they fail, and defines regression tests
and implementation requirements.

The changes below are intentionally not presented as completed fixes. They need
to be implemented and run through the ReturnGuard test suite before merging.

## 1. Range-for reference misclassified as returned stack storage

### Observed diagnostic

```text
src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.cpp:993:13:
address of stack-allocated variable returned
```

The source pattern is:

```cpp
auto& fullscreenTargets = condition ? memberA : memberB;

for (auto& state : fullscreenTargets) {
    if (matches(state))
        return &state;
}
```

`state` is a reference to an element owned by a member container. Returning
`&state` does not return the address of the range-for reference variable; it
returns the address of the referenced container element.

### Exact cause

The failure is in `src/internal/AnalyzerSafety.cpp`, in
`SafetyVisitor::is_stack_address()`.

The address-of branch currently classifies every non-static local `VarDecl` as
stack storage:

```cpp
if (const auto* var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl())) {
    return var->isLocalVarDecl() && !var->isStaticLocal();
}
```

A C++ reference declaration is an alias, not an object whose address is being
returned. A range-for declaration such as `auto& state` is represented as a
local `VarDecl` of reference type, so the current test loses the distinction
between the reference declaration and its referent.

### Required fix

Do not merely suppress all reference variables. This would miss a real stack
escape:

```cpp
int* bad() {
    int local = 0;
    int& alias = local;
    return &alias;
}
```

Instead, trace the reference binding:

1. If the address operand is a non-reference local object, report it.
2. If it is a reference variable, classify the storage denoted by its
   initializer.
3. For a C++ range-for declaration (`VarDecl::isCXXForRangeDecl()`), locate the
   enclosing `CXXForRangeStmt` and classify the range expression:
   - member, global, static, or parameter-owned range: not current-frame stack
     storage;
   - local array, local container, or temporary range: current-frame storage;
   - unknown call/iterator source: remain conservative without claiming that
     the reference declaration itself is a stack object.
4. Preserve recursive handling for pointer aliases such as:

```cpp
int local = 0;
int* pointer = &local;
return pointer;
```

A useful internal separation would be:

```cpp
bool expression_points_into_current_frame(const clang::Expr* expression);
bool declaration_has_current_frame_storage(const clang::VarDecl* variable);
```

The diagnostic should be based on the referent/storage owner, not simply
`VarDecl::isLocalVarDecl()`.

### Regression tests

Add clean cases to `tests/cases/safety.cpp`:

```cpp
#include <vector>

struct item {
    int value;
};

struct owner {
    std::vector<item> items;

    item* find_member_item() {
        for (auto& value : items) {
            if (value.value == 1)
                return &value;
        }
        return nullptr;
    }
};

item* find_parameter_item(std::vector<item>& items) {
    for (auto& value : items) {
        if (value.value == 1)
            return &value;
    }
    return nullptr;
}
```

Keep/add failing cases:

```cpp
int* reference_to_local_is_bad() {
    int local = 0;
    int& alias = local;
    return &alias;
}

item* local_range_is_bad() {
    std::vector<item> items(1);
    for (auto& value : items)
        return &value;
    return nullptr;
}
```

## 2. Smart-pointer guard is not connected to a later `get()` result

### Observed diagnostics

ReturnGuard reported four possible null dereferences in code shaped like:

```cpp
auto it = find(...);
if (it == values.end())
    return;
if (!*it)
    return;

it->get()->field = value;
```

The affected Hyprland files were:

```text
src/managers/XCursorManager.cpp
src/protocols/ColorManagement.cpp
src/protocols/types/SurfaceStateQueue.cpp
```

### Exact cause

The nullable call analysis starts from the raw pointer-producing `get()` call:

- `Analyzer::analyze_nullable_call()` calls
  `NullStateAnalysis::unsafe_dereferences_for(*call)`.
- `unsafe_dereferences_for()` creates `NullSource{.expr = &call}`.
- The dataflow therefore tracks the raw pointer result of `get()` only.

The earlier condition checks the owning smart-pointer object, not the later raw
pointer expression. In the iterator form, the ASTs are also structurally
different:

```text
condition: operator bool(operator*(it))
get owner: operator->(it)
```

`stable_lvalue_key()` currently supports declarations and dot-member access,
but not the equivalent `operator*`/`operator->` iterator projections. The
condition constraint therefore cannot refine the state used for the subsequent
`get()` result.

Relevant implementation files:

```text
src/internal/AnalyzerNullState.cpp
src/internal/NullStateAnalysis.hpp
src/internal/NullStateDriver.inc
src/internal/NullStateModel.inc
src/internal/NullStateTransfer.inc
```

### Required fix

Model a nullable raw-pointer result together with the owner expression that
controls its validity.

One workable design is to extend `NullSource`:

```cpp
struct NullSource {
    const clang::Expr* expr = nullptr;       // raw pointer-producing expression
    const clang::ValueDecl* decl = nullptr;
    const clang::Expr* guard_expr = nullptr; // smart-pointer owner expression
};
```

For a pointer-returning member call named `get`, set `guard_expr` to the
`CXXMemberCallExpr::getImplicitObjectArgument()`.

Then:

1. Normalize equivalent owner expressions in `stable_lvalue_key()`.
   `operator*(iterator)` and `operator->(iterator)` must map to the same logical
   iterator-element key.
2. Recognize contextual boolean conversions such as `operator bool()` as a
   constraint on the implicit object argument.
3. On the non-null branch, refine the owner key to `NonNull`.
4. When evaluating the target `get()` call, inherit the current fact of its
   owner key instead of always resetting the call result to `MaybeNull`.
5. Invalidate the refinement when the owner/iterator/container is reassigned,
   escaped, or observably mutated.

Do not add a name-only suppression such as "all calls named `get` are safe after
any previous `if`". The relationship must be between the same owner expression
and the same control-flow path.

### Regression test

Add a clean iterator-backed case to `tests/cases/null_state.cpp`:

```cpp
struct node {
    int value;
};

template <class T>
struct checked_owner {
    T* pointer;

    explicit operator bool() const { return pointer != nullptr; }
    T* get() RETURNGUARD_FAILS_NULL { return pointer; }
};

template <class T>
struct checked_iterator {
    checked_owner<T>* pointer;

    checked_owner<T>& operator*() const { return *pointer; }
    checked_owner<T>* operator->() const { return pointer; }
};

int guarded_get_through_iterator(checked_iterator<node> it) {
    if (!*it)
        return 0;
    return it->get()->value;
}
```

Keep a failing counterpart:

```cpp
int unguarded_get_through_iterator(checked_iterator<node> it) {
    return it->get()->value;
}
```

Also test invalidation:

```cpp
int reassigned_after_guard(checked_owner<node> owner) {
    if (!owner)
        return 0;
    owner = {};
    return owner.get()->value;
}
```

The last case must continue to warn.

## Implementation priority

1. Fix reference/referent lifetime classification and add the four lifetime
   regression cases.
2. Add owner-aware null-state modeling for direct smart-pointer variables.
3. Extend owner identity to iterator `operator*`/`operator->` projections.
4. Add mutation invalidation before declaring the iterator case fixed.

## Verification required before merging

Run:

```sh
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Then rebuild ReturnGuard and repeat the focused Hyprland scans:

```sh
returnguard --mode=practical -p build-rg \
    src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.cpp

returnguard --mode=practical -p build-rg \
    src/managers/XCursorManager.cpp \
    src/protocols/ColorManagement.cpp \
    src/protocols/types/SurfaceStateQueue.cpp
```

Expected result:

- no false stack-return diagnostic for the member-backed range-for element;
- no null-dereference diagnostic after a dominating smart-owner guard;
- preserved warnings for real local-reference escapes, local-container element
  escapes, unguarded `get()`, and guard invalidation.
