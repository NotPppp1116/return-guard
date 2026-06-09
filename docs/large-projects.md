# Running ReturnGuard on large projects

`returnguard-project` is the compilation-database driver installed alongside
`returnguard`. It runs each translation unit in a separate process and limits
the number of active Clang frontends, so one very large AST is released before
the next batch grows without bound.

Generate `compile_commands.json` first:

```sh
cmake -S . -B build -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

Then run the whole C project:

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

Only `.c` files are selected by default. The driver resolves relative paths,
deduplicates repeated compilation-database entries, sorts them, and skips
missing generated sources.

```sh
returnguard-project -p build \
    --extensions .c,.cc,.cpp \
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

The sorted translation-unit set can be divided deterministically across jobs:

```sh
returnguard-project -p build --shard-count 4 --shard-index 0
returnguard-project -p build --shard-count 4 --shard-index 1
returnguard-project -p build --shard-count 4 --shard-index 2
returnguard-project -p build --shard-count 4 --shard-index 3
```

Each source belongs to exactly one shard after filtering and deduplication.

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

Use `--fail-fast` to stop submitting new translation units after the first
nonzero result. Already-running processes are allowed to finish and their
output is retained.

## Exit status

- `0`: every selected ReturnGuard process succeeded.
- `1`: at least one process reported an error, timed out, or failed to start.
- `2`: invalid runner configuration or compilation database.
- `130`: interrupted by the user.

For findings to produce a nonzero child status, pass
`--fail-on-diagnostics`.
