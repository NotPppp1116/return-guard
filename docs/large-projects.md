# Running ReturnGuard on large projects

`returnguard-project` is the compilation-database driver installed alongside
`returnguard`. It runs each translation unit in a separate process and limits
the number of active Clang frontends, so one very large AST is released before
the next batch grows without bound.

Generate `compile_commands.json` first:

```sh
cmake -S . -B build -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

Then run the whole C or C++ project:

```sh
returnguard-project \
    -p build \
    --mode=strict \
    --fail-on-diagnostics \
    --no-color \
    -j 8
```

The default job count is `min(CPU count, 8)`. Override it with `-j` or the
`RETURNGUARD_JOBS` environment variable. Clang frontends can consume a lot of
memory, so using every hardware thread is not always faster.

## Selecting work

`.c`, `.cc`, `.cpp`, and `.cxx` files are selected by default. The driver
resolves relative paths, deduplicates repeated compilation-database entries,
sorts them, and skips missing generated sources.

```sh
returnguard-project -p build \
    --extensions .c,.cpp \
    --include-regex '/src/' \
    --exclude-regex '/generated/'
```

Inspect the selected set without invoking Clang:

```sh
returnguard-project -p build --list-files
```

Limit a trial run:

```sh
returnguard-project -p build --max-files 100 --dry-run
```

## CI sharding

Translation units are assigned by hashing their path relative to the
compilation database. Adding one source file therefore does not reshuffle every
later source between CI jobs, unlike index-based round-robin sharding.

```sh
returnguard-project -p build --shard-count 4 --shard-index 0
returnguard-project -p build --shard-count 4 --shard-index 1
returnguard-project -p build --shard-count 4 --shard-index 2
returnguard-project -p build --shard-count 4 --shard-index 3
```

Each source belongs to exactly one shard after filtering and deduplication. Use
`--shard-root /path/to/project` when different build directories should produce
the same shard assignment.

## Keeping memory and output bounded

Child output is written to temporary files instead of being retained in RAM.
It is streamed when each translation unit completes. To preserve per-file logs
without printing them:

```sh
returnguard-project -p build \
    --log-dir returnguard-logs \
    --no-stream-output
```

A single pathological translation unit can be stopped with a timeout:

```sh
returnguard-project -p build --timeout 180
```

On POSIX systems, timeout and Ctrl-C cleanup target the entire ReturnGuard
process group, so helper processes are not left running. Ctrl-C also stops queued
work and causes active analyzers to terminate promptly.

Use `--fail-fast` to stop submitting new translation units after the first
nonzero result. Already-running processes are allowed to finish unless the run
is interrupted.

## Exit status

- `0`: every selected ReturnGuard process succeeded.
- `1`: at least one process reported an error, timed out, or failed to start.
- `2`: invalid runner configuration or compilation database.
- `130`: interrupted by the user.

For findings to produce a nonzero child status, pass
`--fail-on-diagnostics`.
