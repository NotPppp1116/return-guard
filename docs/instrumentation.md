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
original value. Because the check is an expression wrapper, nesting and
short-circuit evaluation are preserved.

Calls whose results ReturnGuard already proves handled are not rewritten.
Macro-expanded calls and source ranges outside the main file are currently
skipped rather than rewritten unsafely.

## Runtime

Compile and link the transformed source with the installed static runtime:

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

The prototype recognizes common null-returning allocation and stream APIs, and
common negative-returning POSIX and stdio APIs. Project functions can declare a
contract explicitly:

```c
#include <returnguard/Contracts.h>

void* allocate_packet(size_t size) RETURNGUARD_FAILS_NULL;
int commit_database(void) RETURNGUARD_FAILS_NEGATIVE;
```

`free` and `memcpy` are intentionally not return-result contracts. `free`
belongs to ownership/lifetime analysis, while invalid `memcpy` arguments cause
undefined behavior rather than a reported failure.

## Current boundaries

- Instrumentation currently writes exactly one transformed translation unit per
  invocation.
- Macro-expanded calls are skipped.
- Indirect function-pointer calls require future contract propagation.
- Negative-result checks do not yet model `errno`-specific recovery such as
  `EINTR` or `EAGAIN`.
- `read` and `write` may complete partially. The current wrapper detects only a
  negative result; callers that require complete transfer must still implement
  a loop or an explicit short-count policy.
- A future project driver can mirror a source tree, transform each compilation
  database entry, compile the mirror, retain the final artifacts, and remove
  the temporary sources.
