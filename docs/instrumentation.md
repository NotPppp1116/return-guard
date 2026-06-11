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
int fd = __RG_CHECK_NEGATIVE(open(path, O_RDONLY), 1234567890123456789ULL);
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
C++ sources, links `ReturnGuard::runtime`, and writes per-object metadata beneath:

```text
<current-binary-dir>/returnguard-sites/<target>/
```

The directory is registered with CMake as additional clean output. A project can
select explicit locations when needed:

```cmake
returnguard_harden_target(my_program
    SITE_MAP_DIR "${CMAKE_BINARY_DIR}/security-sites/my_program"
    SOURCE_ROOT "${PROJECT_SOURCE_DIR}")
```

`SOURCE_ROOT` controls path normalization in site IDs and metadata. It should be
a stable project root so separate checkouts produce the same IDs. The target's
chosen metadata directory is also stored in the custom
`RETURNGUARD_SITE_MAP_DIR` target property.

A custom ReturnGuard installation prefix can be selected through
`CMAKE_PREFIX_PATH`:

```sh
cmake -S . -B build-hardened \
    -DCMAKE_PREFIX_PATH=/path/to/returnguard/prefix
cmake --build build-hardened
```

## Site metadata

Manual transformation can emit metadata alongside the generated source:

```sh
returnguard \
    --instrument-output=/tmp/main.returnguard.c \
    --site-map-output=/tmp/main.returnguard-sites.json \
    --site-root="$PWD" \
    src/main.c -- -std=c17
```

Each successfully inserted wrapper has one record:

```json
{
  "schema_version": 1,
  "sites": [
    {
      "id": "1234567890123456789",
      "file": "src/main.c",
      "line": 42,
      "column": 14,
      "function": "load_configuration",
      "callee": "open",
      "callee_type": "int (const char *, int, ...)",
      "predicate": "negative"
    }
  ]
}
```

The ID is stored as a decimal string so JSON consumers do not lose precision.
ReturnGuard currently computes a 64-bit FNV-1a hash over the normalized file,
line, column, enclosing function, qualified callee name, canonical callee type,
and failure predicate. Including the canonical type distinguishes C++ overloads
that share the same qualified name. A zero hash is reserved. Different metadata
producing the same ID is a hard transformation error within a translation unit
and a hard merge error across translation units.

Per-object maps can be combined after a build:

```sh
returnguard-site-map \
    --input-dir build/returnguard-sites/my_program \
    --output build/my_program.returnguard-sites.json
```

The merger validates the schema, unsigned 64-bit ID range, canonical callee type,
predicate names, ID collisions, and inconsistent IDs assigned to the same logical
site. It sorts records by numeric ID and replaces the combined output atomically.
The requested output is removed before validation, so a failed merge cannot leave
an older map looking current.

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

For normal compile commands with an explicit `-o`, the launcher:

1. creates a unique temporary directory beside the object output, not in the
   source tree;
2. writes the transformed source there;
3. restores the original quoted-include directory with `-iquote`;
4. optionally creates a deterministic per-object site map;
5. compiles the transformed source;
6. rewrites dependency output to name the original source;
7. preserves original `__FILE__`, macro, diagnostic, and debug paths;
8. removes the temporary directory even when compilation fails;
9. removes the site map when the real compiler does not produce the object.

This allows hardened builds from read-only, vendored, or otherwise immutable
source trees. Commands without an explicit output retain the older adjacent
source fallback because no writable build-output directory can be identified.

The launcher recognizes these optional environment variables:

- `RETURNGUARD_TOOL`: explicit path to the ReturnGuard executable;
- `RETURNGUARD_INCLUDE_DIR`: directory containing `returnguard/Runtime.h`;
- `RETURNGUARD_SITE_MAP_DIR`: directory for per-object JSON maps;
- `RETURNGUARD_SOURCE_ROOT`: root used to normalize source paths;
- `RETURNGUARD_DISABLE=1`: pass the compilation through unchanged.

When `RETURNGUARD_SITE_MAP_DIR` is set, compile commands must provide an explicit
`-o` object path. Unsupported response-file and standard-input source
compilations fail closed rather than silently compiling an uninstrumented
source.

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

void __rg_fatal_hook(uint64_t site_id, int saved_errno) {
    /* Look up site_id in the merged JSON map. */
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

Built-in contracts are limited to declarations from system headers in the
legitimate global scope, plus `std` and its inline namespaces for standard C
library APIs. A same-named function in another namespace does not inherit libc
or POSIX failure semantics. Explicit ReturnGuard annotations still work in any
namespace.

The automatic null contracts currently cover stream and directory APIs whose
null result is unambiguously failure, such as `fopen`, `freopen`, `tmpfile`,
`fdopen`, and `opendir`. Project functions can declare contracts explicitly:

```c
#include <returnguard/Contracts.h>

void* allocate_nonzero_packet(size_t size) RETURNGUARD_FAILS_NULL;
int commit_database(void) RETURNGUARD_FAILS_NEGATIVE;
```

An explicitly annotated allocation wrapper must reject zero sizes or otherwise
define every null result as failure. A null contract requires a pointer return
type. A negative contract requires a signed integer return type. Invalid
declarations stop transformation with an error instead of silently bypassing
the contract.

`malloc`, `calloc`, `aligned_alloc`, and `realloc` are not automatically wrapped.
A zero-size request may legally return null, so they require a future size-aware
rewrite that preserves single evaluation of every argument. A project may
instead annotate a wrapper whose own contract makes null unambiguously fatal,
for example because it rejects zero sizes before calling the allocator.

`free` and `memcpy` are intentionally not return-result contracts. `free`
belongs to ownership/lifetime analysis, while invalid `memcpy` arguments cause
undefined behavior rather than a reported failure.

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
- C and C++ compilers must support the quoted-include and path-remapping options
  used by the launcher; current testing targets modern GCC and Clang on Linux.
- After changing a target's source list, clean its build outputs before treating
  a manually merged site map as authoritative.
