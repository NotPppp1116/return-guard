#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import shlex
import shutil
import sys
import tempfile
import threading
import time
from typing import Iterable, Pattern, Sequence

from returnguard_project_runtime import (
    RunResult,
    TranslationUnit,
    bounded_results,
    build_command,
    copy_or_print_output,
    internal_error_result,
    run_translation_unit,
)


DEFAULT_EXTENSIONS = (".c",)
DEFAULT_JOB_LIMIT = 8


class RunnerError(RuntimeError):
    pass


def default_jobs() -> int:
    configured = os.environ.get("RETURNGUARD_JOBS")
    if configured is not None:
        try:
            value = int(configured)
        except ValueError as error:
            raise RunnerError("RETURNGUARD_JOBS must be an integer") from error
        if value < 1:
            raise RunnerError("RETURNGUARD_JOBS must be at least 1")
        return value

    return max(1, min(os.cpu_count() or 1, DEFAULT_JOB_LIMIT))


def compile_database_path(value: pathlib.Path) -> pathlib.Path:
    value = value.expanduser()
    if value.is_dir():
        value = value / "compile_commands.json"
    return value.resolve()


def load_translation_units(database: pathlib.Path) -> list[TranslationUnit]:
    try:
        raw = json.loads(database.read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise RunnerError(f"compilation database not found: {database}") from error
    except OSError as error:
        raise RunnerError(f"could not read compilation database {database}: {error}") from error
    except json.JSONDecodeError as error:
        raise RunnerError(
            f"invalid JSON in {database}:{error.lineno}:{error.colno}: {error.msg}"
        ) from error

    if not isinstance(raw, list):
        raise RunnerError(f"{database} must contain a JSON array")

    unique: dict[str, TranslationUnit] = {}
    for index, entry in enumerate(raw):
        if not isinstance(entry, dict):
            raise RunnerError(f"entry {index} in {database} is not an object")

        file_value = entry.get("file")
        if not isinstance(file_value, str) or not file_value:
            raise RunnerError(f"entry {index} in {database} has no valid 'file'")

        directory_value = entry.get("directory", str(database.parent))
        if not isinstance(directory_value, str) or not directory_value:
            raise RunnerError(
                f"entry {index} in {database} has no valid 'directory'"
            )

        directory = pathlib.Path(directory_value).expanduser()
        if not directory.is_absolute():
            directory = database.parent / directory
        directory = directory.resolve(strict=False)

        source = pathlib.Path(file_value).expanduser()
        if not source.is_absolute():
            source = directory / source
        source = source.resolve(strict=False)
        unique.setdefault(str(source), TranslationUnit(source))

    return sorted(unique.values(), key=lambda unit: unit.source.as_posix())


def compile_patterns(values: Sequence[str], option: str) -> tuple[Pattern[str], ...]:
    patterns: list[Pattern[str]] = []
    for value in values:
        try:
            patterns.append(re.compile(value))
        except re.error as error:
            raise RunnerError(
                f"invalid {option} regular expression {value!r}: {error}"
            ) from error
    return tuple(patterns)


def parse_extensions(value: str) -> frozenset[str]:
    extensions: set[str] = set()
    for item in value.split(","):
        item = item.strip().lower()
        if not item:
            continue
        extensions.add(item if item.startswith(".") else "." + item)
    if not extensions:
        raise RunnerError("--extensions must contain at least one extension")
    return frozenset(extensions)


def shard_key(source: pathlib.Path, shard_root: pathlib.Path) -> str:
    try:
        relative = os.path.relpath(source, shard_root)
    except ValueError:
        relative = source.as_posix()
    return pathlib.PurePath(relative).as_posix()


def shard_for_source(
    source: pathlib.Path,
    *,
    shard_root: pathlib.Path,
    shard_count: int,
) -> int:
    key = shard_key(source, shard_root).encode("utf-8", errors="surrogateescape")
    digest = hashlib.blake2b(
        key,
        digest_size=8,
        person=b"returnguard",
    ).digest()
    return int.from_bytes(digest, byteorder="big") % shard_count


def select_translation_units(
    units: Iterable[TranslationUnit],
    *,
    extensions: frozenset[str],
    include_patterns: Sequence[Pattern[str]],
    exclude_patterns: Sequence[Pattern[str]],
    include_missing: bool,
    shard_root: pathlib.Path,
    shard_index: int,
    shard_count: int,
    max_files: int | None,
) -> tuple[list[TranslationUnit], int]:
    selected: list[TranslationUnit] = []
    missing = 0

    for unit in units:
        source_text = unit.source.as_posix()
        if unit.source.suffix.lower() not in extensions:
            continue
        if include_patterns and not any(
            pattern.search(source_text) for pattern in include_patterns
        ):
            continue
        if any(pattern.search(source_text) for pattern in exclude_patterns):
            continue
        if not include_missing and not unit.source.is_file():
            missing += 1
            continue
        if (
            shard_for_source(
                unit.source,
                shard_root=shard_root,
                shard_count=shard_count,
            )
            != shard_index
        ):
            continue
        selected.append(unit)

    if max_files is not None:
        selected = selected[:max_files]
    return selected, missing


def discover_tool(value: str | None) -> str:
    if value:
        candidate = pathlib.Path(value).expanduser()
        if candidate.is_absolute() or candidate.parent != pathlib.Path("."):
            if not candidate.is_file():
                raise RunnerError(f"ReturnGuard executable not found: {candidate}")
            if not os.access(candidate, os.X_OK):
                raise RunnerError(
                    f"ReturnGuard executable is not executable: {candidate}"
                )
            return str(candidate.resolve())

        resolved = shutil.which(value)
        if resolved is None:
            raise RunnerError(f"ReturnGuard executable not found in PATH: {value}")
        return resolved

    sibling = pathlib.Path(sys.argv[0]).resolve().with_name("returnguard")
    if sibling.is_file() and os.access(sibling, os.X_OK):
        return str(sibling)

    resolved = shutil.which("returnguard")
    if resolved is None:
        raise RunnerError(
            "ReturnGuard executable not found; pass --tool or add returnguard to PATH"
        )
    return resolved


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        prog="returnguard-project",
        description=(
            "Run ReturnGuard across a compilation database with bounded parallelism."
        ),
    )
    result.add_argument(
        "-p",
        "--build-path",
        required=True,
        type=pathlib.Path,
        help="build directory or compile_commands.json path",
    )
    result.add_argument("--tool", help="ReturnGuard executable (default: sibling or PATH)")
    result.add_argument("-j", "--jobs", type=int, help="parallel analyzer processes")
    result.add_argument(
        "--mode",
        choices=("practical", "strict", "ignored-only"),
        default="practical",
    )
    result.add_argument("--fail-on-diagnostics", action="store_true")
    result.add_argument("--analyze-headers", action="store_true")
    result.add_argument("--include-operators", action="store_true")
    result.add_argument("--include-reference-returns", action="store_true")
    result.add_argument("--no-color", action="store_true")
    result.add_argument(
        "--extensions",
        default=",".join(DEFAULT_EXTENSIONS),
        help="comma-separated source extensions (default: .c)",
    )
    result.add_argument("--include-regex", action="append", default=[])
    result.add_argument("--exclude-regex", action="append", default=[])
    result.add_argument("--include-missing", action="store_true")
    result.add_argument("--shard-index", type=int, default=0)
    result.add_argument("--shard-count", type=int, default=1)
    result.add_argument(
        "--shard-root",
        type=pathlib.Path,
        help="root used to derive stable relative paths for CI sharding",
    )
    result.add_argument("--max-files", type=int)
    result.add_argument("--fail-fast", action="store_true")
    result.add_argument("--timeout", type=float, help="per-file timeout in seconds")
    result.add_argument("--log-dir", type=pathlib.Path)
    result.add_argument(
        "--no-stream-output",
        action="store_true",
        help="write diagnostics only to --log-dir instead of stdout",
    )
    result.add_argument("--list-files", action="store_true")
    result.add_argument("--dry-run", action="store_true")
    result.add_argument("--verbose", action="store_true")
    result.add_argument(
        "--progress-every",
        type=int,
        default=100,
        help="print progress every N completed files; 0 disables progress",
    )
    result.add_argument(
        "--tool-arg",
        action="append",
        default=[],
        help="extra argument passed to every ReturnGuard process",
    )
    return result


def validate_arguments(arguments: argparse.Namespace) -> int:
    jobs = arguments.jobs if arguments.jobs is not None else default_jobs()
    if jobs < 1:
        raise RunnerError("--jobs must be at least 1")
    if arguments.shard_count < 1:
        raise RunnerError("--shard-count must be at least 1")
    if not 0 <= arguments.shard_index < arguments.shard_count:
        raise RunnerError("--shard-index must be in [0, shard-count)")
    if arguments.max_files is not None and arguments.max_files < 1:
        raise RunnerError("--max-files must be at least 1")
    if arguments.progress_every < 0:
        raise RunnerError("--progress-every cannot be negative")
    if arguments.timeout is not None and arguments.timeout <= 0:
        raise RunnerError("--timeout must be greater than zero")
    if arguments.no_stream_output and arguments.log_dir is None:
        raise RunnerError("--no-stream-output requires --log-dir")
    return jobs


def main(argv: Sequence[str] | None = None) -> int:
    arguments = parser().parse_args(argv)

    try:
        jobs = validate_arguments(arguments)
        database = compile_database_path(arguments.build_path)
        all_units = load_translation_units(database)
        shard_root = (
            arguments.shard_root.expanduser().resolve()
            if arguments.shard_root is not None
            else database.parent
        )
        selected, missing = select_translation_units(
            all_units,
            extensions=parse_extensions(arguments.extensions),
            include_patterns=compile_patterns(
                arguments.include_regex,
                "--include-regex",
            ),
            exclude_patterns=compile_patterns(
                arguments.exclude_regex,
                "--exclude-regex",
            ),
            include_missing=arguments.include_missing,
            shard_root=shard_root,
            shard_index=arguments.shard_index,
            shard_count=arguments.shard_count,
            max_files=arguments.max_files,
        )

        if arguments.list_files:
            for unit in selected:
                print(unit.source)
            return 0

        if not selected:
            print(
                "returnguard-project: no matching translation units",
                file=sys.stderr,
            )
            return 0

        tool = discover_tool(arguments.tool)
        common_arguments = dict(
            tool=tool,
            database_directory=database.parent,
            mode=arguments.mode,
            fail_on_diagnostics=arguments.fail_on_diagnostics,
            analyze_headers=arguments.analyze_headers,
            include_operators=arguments.include_operators,
            include_reference_returns=arguments.include_reference_returns,
            no_color=arguments.no_color,
            tool_arguments=tuple(arguments.tool_arg),
        )

        if arguments.dry_run:
            for unit in selected:
                print(
                    shlex.join(
                        build_command(
                            source=unit.source,
                            **common_arguments,
                        )
                    )
                )
            return 0

        started = time.monotonic()
        completed_count = 0
        failed_count = 0
        timed_out_count = 0
        internal_error_count = 0
        stop_event = threading.Event()

        with tempfile.TemporaryDirectory(prefix="returnguard-project-") as temporary:
            output_directory = pathlib.Path(temporary)

            def worker(unit: TranslationUnit) -> RunResult:
                try:
                    return run_translation_unit(
                        unit,
                        output_directory=output_directory,
                        timeout_seconds=arguments.timeout,
                        stop_event=stop_event,
                        **common_arguments,
                    )
                except Exception:
                    command = build_command(
                        source=unit.source,
                        **common_arguments,
                    )
                    return internal_error_result(
                        unit,
                        output_directory=output_directory,
                        command=command,
                    )

            results = bounded_results(
                selected,
                jobs=min(jobs, len(selected)),
                worker=worker,
                fail_fast=arguments.fail_fast,
                stop_event=stop_event,
            )
            try:
                for run_result in results:
                    completed_count += 1
                    if run_result.returncode != 0:
                        failed_count += 1
                    if run_result.timed_out:
                        timed_out_count += 1
                    if run_result.internal_error:
                        internal_error_count += 1

                    if arguments.verbose:
                        print(
                            f"returnguard-project: [{completed_count}/{len(selected)}] "
                            f"{run_result.source} ({run_result.elapsed_seconds:.2f}s, "
                            f"exit {run_result.returncode})",
                            file=sys.stderr,
                        )
                        print(
                            "returnguard-project: command: "
                            + shlex.join(run_result.command),
                            file=sys.stderr,
                        )

                    copy_or_print_output(
                        run_result,
                        log_directory=(
                            arguments.log_dir.expanduser().resolve()
                            if arguments.log_dir is not None
                            else None
                        ),
                        stream_output=not arguments.no_stream_output,
                    )

                    if (
                        arguments.progress_every > 0
                        and completed_count % arguments.progress_every == 0
                    ):
                        print(
                            f"returnguard-project: completed {completed_count}/"
                            f"{len(selected)} translation units",
                            file=sys.stderr,
                        )
            except KeyboardInterrupt:
                stop_event.set()
                results.close()
                raise

        elapsed = time.monotonic() - started
        stopped_early = completed_count < len(selected)
        print(
            "returnguard-project: "
            f"analyzed {completed_count}/{len(selected)} translation units "
            f"in {elapsed:.2f}s with {min(jobs, len(selected))} job(s); "
            f"{failed_count} failed; {timed_out_count} timed out; "
            f"{internal_error_count} internal worker error(s); "
            f"{missing} missing source file(s) skipped"
            + ("; stopped early" if stopped_early else ""),
            file=sys.stderr,
        )
        return 1 if failed_count else 0

    except RunnerError as error:
        print(f"returnguard-project: error: {error}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("returnguard-project: interrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
