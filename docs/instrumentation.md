# Fail-closed call instrumentation

ReturnGuard can write a transformed copy of one C or C++ translation unit in
which unchecked calls with known failure contracts are wrapped by a
value-preserving check.

```sh
returnguard \
    --instrument-output=/tmp/main.returnguard.c \
    src/main.c -- -std=c17
```

The original source is never modified. A call such as:

```c
int fd = open(path, O_RDONLY);
```

is emitted as:

```c
int fd = __RG_CHECK_NEGATIVE(open(path, O_RDONLY), 178392114u);
```

The wrapper evaluates the call once, terminates through the shared
`__rg_fatal` routine if the result indicates failure, and otherwise returns the
original value. Because the check is an expression wrapper, nesting,
short-circuit evaluation, pointer types, and integer types are preserved.

Calls whose results ReturnGuard already proves handled are not rewritten.
Macro-expanded calls and source ranges outside the main file are currently
skipped rather than rewritten unsafely.

## Transparent compiler launcher

The installed `returnguard-compiler-launcher` can instrument each translation
unit immediately before the real compiler processes it. CMake compiler launchers
receive the compiler executable as their first argument, which matches this
script's interface:

```sh
cmake -S . -B build-hardened \
    -DCMAKE_C_COMPILER_LAUNCHER=returnguard-compiler-launcher \
    -DCMAKE_CXX_COMPILER_LAUNCHER=returnguard-compiler-launcher

cmake --build build-hardened
```

The launcher:

1. creates a temporary transformed source beside the original source;
2. invokes ReturnGuard with the real compilation arguments;
3. compiles the transformed source;
4. rewrites dependency output to name the original source;
5. preserves the original path for `__FILE__` and diagnostics with `#line`;
6. removes the temporary source even when compilation fails.

The final program must still link `returnguard_runtime`. For CMake projects,
link the installed library to each hardened executable or shared library:

```cmake
target_link_libraries(my_program PRIVATE returnguard_runtime)
```

The launcher recognizes these optional environment variables:

- `RETURNGUARD_TOOL`: explicit path to the ReturnGuard executable;
- `RETURNGUARD_INCLUDE_DIR`: directory containing `returnguard/Runtime.h`;
- `RETURNGUARD_DISABLE=1`: pass the compilation through unchanged.

## Runtime

Compile and link a manually transformed source with the installed static
runtime:

```sh
cc -I/usr/local/include -c /tmp/main.returnguard.c -o main.o
cc main.o -L/usr/local/lib -lreturnguard_runtime -o program
```

The shared failure entry point is declared `noreturn`, `cold`, `noinline`, and
hidden. Its default behavior invokes a weak application hook and then calls
`_Exit(127)`. Applications may provide a strong hook definition:

```c
#include <returnguard/Runtime.h>

void __rg_fatal_hook(uint32_t site_id, int saved_errno) {
    /* Best-effort reporting and wiping of explicitly tracked secrets only. */
}
```

The hook must not attempt to resume execution.

## Built-in contracts

The prototype recognizes straightforward null-returning allocation and stream
APIs, and straightforward negative-returning POSIX and stdio APIs. Project
functions can declare a contract explicitly:

```c
#include <returnguard/Contracts.h>

void* allocate_packet(size_t size) RETURNGUARD_FAILS_NULL;
int commit_database(void) RETURNGUARD_FAILS_NEGATIVE;
```

`free` and `memcpy` are intentionally not return-result contracts. `free`
belongs to ownership/lifetime analysis, while invalid `memcpy` arguments cause
undefined behavior rather than a reported failure.

`realloc` is also not automatically wrapped yet because a zero-size call may
legally return null without representing a normal allocation failure.

Context-sensitive I/O functions such as `read`, `write`, `recv`, `send`, and
`accept` are not automatically fatal. They need policies for `EINTR`, `EAGAIN`,
nonblocking descriptors, EOF, and partial transfers. A project may explicitly
mark a wrapper around one of those operations once it has selected the desired
policy.

## Current boundaries

- Manual `--instrument-output` mode writes one transformed translation unit per
  invocation.
- The compiler launcher supports one source file per compile command, matching
  normal CMake and Ninja compilation rules.
- Macro-expanded calls are skipped.
- Indirect function-pointer calls require future contract propagation.
- GCC-specific compilation options unsupported by Clang may require filtering or
  a Clang-based hardened build.
- Linking the runtime is still an explicit build-system step.
