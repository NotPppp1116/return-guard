# ReturnGuard C/C++

ReturnGuard is a standalone Clang LibTooling analyzer for C and C++ call-site return
handling. It reports non-`void` calls whose result is ignored, consumed without
verification, or only partially checked.

The analyzer executable is implemented in C++ because Clang's native tooling
API is C++. It can analyze ordinary C (`.c` and `.h`) and C++ (`.cc`, `.cpp`,
and `.cxx`) translation units.

## What it currently understands

- Calls returning any non-`void` type.
- Native must-check signals such as `__attribute__((warn_unused_result))`,
  `[[nodiscard]]`, and project macros built from those attributes.
- `_Bool` and enum return domains.
- Finite integer domains supplied through `annotate` attributes on any function
  redeclaration.
- Explicitly nullable pointer returns, with path-sensitive checks before `*`,
  `->`, array subscripting, and indirect function calls.
- Direct `switch (function())` and `if (function())` checks.
- Results stored in a simple local variable and checked later.
- `if`/`else if` chains using `==`, `!=`, `<`, `<=`, `>`, `>=`, `&&`, `||`,
  and `!` when the conditions can be evaluated for a known finite domain.
- `switch` cases, GNU case ranges, and exhaustive `default`.
- Explicit discards such as `(void)function()`.
- Calls and constants created by function-like and object-like macros.
- Compilation databases, include directories, command-line definitions, and
  the active preprocessor configuration supplied by the real build.

## Macro handling

ReturnGuard does not implement a separate preprocessor. Clang preprocesses the
translation unit before building the AST, so a macro such as:

```c
#define GET_STATUS() read_status()
#define STATUS_OK 1

int status = GET_STATUS();
if (status == STATUS_OK) {
    /* ... */
}
```

appears to the analyzer as a real call expression and a constant comparison.
Diagnostics are mapped back to the macro invocation or argument in the user's
source file with Clang's `SourceManager`.

Only the active branch of `#if`/`#ifdef` is analyzed, matching the actual
compilation.

## Modes

### `practical` (default)

Reports ignored results and incomplete checks when ReturnGuard knows a finite
return domain.

```sh
returnguard --mode=practical file.c -- -std=c17
```

### `strict`

Requires every non-`void` result to be checked exhaustively, returned onward,
or explicitly discarded. Open-ended integer-like types need a final `else`,
`switch default`, or an explicit finite domain annotation before ReturnGuard can
prove exhaustive checking.

```sh
returnguard --mode=strict file.c -- -std=c17
```

### `ignored-only`

Only reports completely ignored results.

```sh
returnguard --mode=ignored-only file.c -- -std=c17
```

## CI and diagnostic options

By default, ReturnGuard findings are warnings and the tool can still exit
successfully. Use `--fail-on-diagnostics` when a finding should fail CI:

```sh
returnguard --mode=strict --fail-on-diagnostics \
    -p build src/main.c
```

This promotes ReturnGuard findings to Clang errors, causing the tool to return a
nonzero status. Use `--no-color` for stable plain-text logs:

```sh
returnguard --no-color file.c -- -std=c17
```

## Build on Arch Linux

Install LLVM, Clang, CMake, and Ninja:

```sh
sudo pacman -S llvm clang cmake ninja
```

Configure and build:

```sh
cmake -S . -B build -G Ninja \
    -DCMAKE_CXX_COMPILER=clang++
cmake --build build
```

Run the regression tests:

```sh
ctest --test-dir build --output-on-failure
```

## Run on one file

```sh
./build/returnguard tests/cases/macros.c -- \
    -std=c17 \
    -Itests/include
```

Arguments after `--` are passed to Clang as compilation arguments.
For C++ files, pass the project's C++ frontend flags, for example
`-std=c++20`.

## Run against a CMake project

Generate `compile_commands.json` in the target project:

```sh
cmake -S /path/to/project -B /path/to/project/build \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

Then reuse its exact compilation command:

```sh
./build/returnguard \
    --mode=strict \
    -p /path/to/project/build \
    /path/to/project/src/main.cpp
```

For large compilation databases, install and use `returnguard-project`; see
[`docs/large-projects.md`](docs/large-projects.md).

The compilation database supplies include paths, `-D` definitions, language
standard, target options, and other frontend flags.

## Describing return values from a declaration

Annotate finite integer domains:

```c
#if defined(__clang__) || defined(__GNUC__)
#define RG_VALUES(...) \
    __attribute__((annotate("returnguard.values:" #__VA_ARGS__)))
#else
#define RG_VALUES(...)
#endif

RG_VALUES(1, 4, 32)
int read_status(void);
```

ReturnGuard reads the annotation as the set `{1, 4, 32}`. The annotation may be
placed on any declaration in the function's redeclaration chain, so it can live
in a public header while the definition remains unchanged. Signed values and an
explicit leading `+` are accepted.

For enums, no annotation is needed:

```c
enum error {
    ERROR_OK,
    ERROR_IO,
    ERROR_PERMISSION,
};

enum error load_file(void);
```

## Nullable pointer returns

Mark a pointer-returning API with Clang's native `_Nullable` qualifier, or use
the installed helper macro:

```c
#include <returnguard/Nullability.h>

struct item;
struct item* RETURNGUARD_NULLABLE find_item(int id);
```

ReturnGuard follows the result and its local aliases through the Clang CFG. A
null guard must dominate the dereference on the current path:

```c
struct item* item = find_item(7);
if (item == NULL) {
    return -1;
}
return item->value;
```

See [`docs/nullability.md`](docs/nullability.md) for supported guards,
short-circuit behavior, annotations, and current boundaries.

## Function Contracts And Project Policy

ReturnGuard understands known failure-returning APIs and can be taught
project-specific functions without changing source. Built-in contracts stay
conservative and focus on stable C, POSIX, stdio, and similar system APIs:

```sh
returnguard \
    --contract=project_open=negative \
    --contract=Project::make_handle=null \
    file.c -- -std=c17
```

For larger projects, put one contract per line in a file:

```text
# contracts.rg
project_open=negative
Project::make_handle=null
```

Then pass it with:

```sh
returnguard --contract-file=contracts.rg file.c -- -std=c17
```

For a single project policy file that also teaches the safety analysis about
custom lifetime functions, use `--function-config`:

```text
# returnguard.rg
contract project_open negative
contract Project::make_handle null
lifetime project_alloc alloc
lifetime project_realloc realloc
lifetime project_free free
```

```sh
returnguard --function-config=returnguard.rg file.c -- -std=c17
```

Use `negative` for signed integer APIs that fail with a value below zero, and
`null` for pointer APIs where `NULL` means failure. Source annotations with
`RETURNGUARD_FAILS_NULL` and `RETURNGUARD_FAILS_NEGATIVE` still work and take
precedence when you control the declaration.

## Important limitations

This is not a complete proof system for C or C++.

- An arbitrary `int` function can return more values than static analysis can
  enumerate. Visible integer-returning bodies are not treated as closed finite
  domains; use `returnguard.values` annotations when the API really has a small
  finite result set.
- Finite-value return tracking is local and largely syntactic. Nullable-pointer
  tracking is intraprocedural and path-sensitive over Clang's CFG.
- Values stored in aggregates, passed through callbacks that mutate aliases, or
  recovered through unknown memory may require future dataflow support.
- Function-pointer calls expose the return type, but usually not a finite value
  domain unless the type itself is finite.
- A final `else` or `switch default` is treated as exhaustive by convention. It
  proves fallback coverage, not that each value has unique behavior.

## Source layout

```text
include/returnguard/       public frontend/options API
scripts/                   whole-project driver
src/internal/Model.*       analysis data model
src/internal/AstUtils.*    AST and constant-expression helpers
src/internal/Condition*    symbolic condition evaluation
src/internal/Analyzer*     analysis divided by responsibility
src/internal/Handler*      searches for later checks of stored results
src/Frontend.cpp           Clang frontend action and AST consumer
src/main.cpp               command-line interface
```

The implementation is intentionally divided into small files so individual
parts can be edited and reviewed without touching a single large analyzer file.
