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
Directly returning a result is treated as error propagation rather than as an
unchecked local use. Macro-expanded calls and source ranges outside the main
file are currently skipped rather than rewritten unsafely.

## CMake package integration

After installing ReturnGuard, a CMake project can harden one target with:

```cmake
find_package(ReturnGuard CONFIG REQUIRED)

add_executable(my_program main.c)
returnguard_harden_target(my_program)
```

`returnguard_harden_target()` attaches the installed compiler launcher for C and
C++ sources and links `ReturnGuard::runtime`. The project's normal source tree
remains unchanged.

A custom installation prefix can be selected through `CMAKE_PREFIX_PATH`:

```sh
cmake -S . -B build-hardened \
    -DCMAKE_PREFIX_PATH=/path/to/returnguard/prefix
cmake --build build-hardened
```

## Transparent compiler launcher

The installed `returnguard-compiler-launcher` can also be configured directly.
CMake compiler launchers receive the compiler executable as their first
argument, which matches this script's interface:

```sh
cmake -S . -B build-hardened \
    -DCMAKE_C_COMPILER_LAUNCHER=returnguard-compiler-launcher \
    -DCMAKE_CXX_COMPILER_LAUNCHER=returnguard-compiler-launcher

cmake --build build-hardened
```

When configured manually, the final targets must also link the installed
`ReturnGuard::runtime` target or `libreturnguard_runtime.a`.

The launcher:

1. creates a temporary transformed source beside the original source;
2. invokes ReturnGuard with the real compilation arguments;
3. compiles the transformed source;
4. rewrites dependency output to name the original source;
5. preserves original `__FILE__`, macro, diagnostic, and debug paths;
6. removes the temporary source even when compilation fails.

The launcher recognizes these optional environment variables:

- `RETURNGUARD_TOOL`: explicit path to the ReturnGuard executable;
- `RETURNGUARD_INCLUDE_DIR`: directory containing `returnguard/Runtime.h`;
- `RETURNGUARD_DISABLE=1`: pass the compilation through unchanged.

Unsupported response-file and standard-input source compilations fail closed
rather than silently compiling an uninstrumented source.

## Runtime

Compile and link a manually transformed source with the installed static
runtime:

```sh
cc -I/usr/local/include -c /tmp/main.returnguard.c -o main.o
cc main.o -L/usr/local/lib -lreturnguard_runtime -o program
```

The shared failure entry point is declared `noreturn`, `cold`, `noinline`, and
hidden. Its default behavior wipes registered secret regions, invokes a weak
application hook, and then calls `_Exit(127)`. Applications may provide a strong
hook definition:

```c
#include <returnguard/Runtime.h>

void __rg_fatal_hook(uint32_t site_id, int saved_errno) {
    /* Registered secret regions have already been wiped here. */
    /* Perform only minimal best-effort reporting. */
}
```

The hook must not attempt to resume execution.

## Explicit secret regions

The runtime keeps a fixed-capacity, allocation-free registry of memory regions
that should be zeroed before the fatal hook runs:

```c
#include <returnguard/Runtime.h>

unsigned char key[32];

if (returnguard_register_secret(key, sizeof(key)) != RETURNGUARD_SECRET_OK) {
    /* Do not place a secret in an unregistered region. */
    return -1;
}

/* Fill and use key. */

/* Explicitly wipe key during normal cleanup, then unregister it. */
if (returnguard_unregister_secret(key) != RETURNGUARD_SECRET_OK) {
    return -1;
}
```

Register a region before writing sensitive bytes into it. Unregister it before
freeing or invalidating the storage, and only after normal-path code has already
made the contents non-sensitive. Duplicate registrations, invalid regions, a
full registry, unknown regions, and a fatal wipe already in progress are
reported through `ReturnGuardSecretResult`.

Wiping is best effort. It covers the exact registered byte ranges, but it cannot
promise to erase copies in registers, compiler temporaries, scratch stack
storage, swap, core dumps, shared memory, or other processes. Registration and
unregistration are thread-safe normal-path operations, but they are not intended
for signal handlers. The fatal path does not acquire the registry update lock.

## Built-in contracts

The prototype recognizes straightforward null-returning allocation and stream
APIs, and straightforward negative-returning POSIX and stdio APIs. Project
functions can declare a contract explicitly:

```c
#include <returnguard/Contracts.h>

void* allocate_packet(size_t size) RETURNGUARD_FAILS_NULL;
int commit_database(void) RETURNGUARD_FAILS_NEGATIVE;
```

A null contract requires a pointer return type. A negative contract requires a
signed integer return type. Invalid declarations stop transformation with an
error instead of silently bypassing the contract.

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
- C and C++ compilers must support the path-remapping options used by the
  launcher; current testing targets modern GCC and Clang on Linux.
