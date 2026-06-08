# ReturnGuard C

ReturnGuard is a standalone Clang LibTooling analyzer for C call-site return
handling. It reports non-`void` calls whose result is ignored, consumed without
verification, or only partially checked.

The analyzer executable is implemented in C++ because Clang's native tooling
API is C++, but the analyzed source is ordinary C (`.c` and `.h`).

## What it currently understands

- Calls returning any non-`void` type.
- `_Bool` and enum return domains.
- Finite integer domains inferred from visible function bodies.
- Finite integer domains supplied through `annotate` attributes on any function
  redeclaration.
- Direct `switch (function())` and `if (function())` checks.
- Results stored in a local variable and checked later.
- `if`/`else if` chains using `==`, `!=`, `<`, `<=`, `>`, `>=`, `&&`, `||`,
  and `!` when the conditions can be evaluated for a known finite domain.
- `switch` cases, GNU case ranges, and `default`.
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
or explicitly discarded. Open-ended types need a final `else`/`default`.

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

## Run on one C file

```sh
./build/returnguard tests/cases/macros.c -- \
    -std=c17 \
    -Itests/include
```

Arguments after `--` are passed to Clang as compilation arguments.

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
    /path/to/project/src/main.c
```

The compilation database supplies include paths, `-D` definitions, language
standard, target options, and other frontend flags.

## Describing return values from a declaration

When a function body is unavailable, annotate a finite integer domain:

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

## Important limitations

This is not a complete proof system for C.

- An arbitrary `int` function can return more values than static analysis can
  enumerate. In strict mode, use a final `else` or `default` for open domains.
- The current variable tracking is local and syntactic, not a full
  path-sensitive whole-program analysis.
- Values copied through several aliases, stored in aggregates, passed through
  callbacks, or modified between checks may require future dataflow support.
- Function-pointer calls expose the return type, but usually not a finite value
  domain unless the type itself is finite.
- A `default` proves coverage, not that each value has unique behavior.

## Source layout

```text
include/returnguard/       public frontend/options API
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
