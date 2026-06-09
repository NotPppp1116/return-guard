#!/usr/bin/env python3

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import Iterable, Iterator, Pattern, Sequence


DEFAULT_EXTENSIONS = (".c",)
DEFAULT_JOB_LIMIT = 8


@dataclass(frozen=True)
class TranslationUnit:
    source: pathlib.Path


@dataclass(frozen=True)
class RunResult:
    source: pathlib.Path
    command: tuple[str, ...]
    returncode: int
    output: str
    elapsed_seconds: float


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
        source = pathlib.Path(file_value).expanduser()
        if not source.is_absolute():
            source = directory / source
        source = source.resolve(strict=False)
        unique.setdefault(str(source), TranslationUnit(source=source))

    return sorted(unique.values(), key=lambda unit: unit.source.as_posix())


def compile_patterns(values: Sequence[str], option: str) -> tuple[Pattern[str], ...]:
    patterns: list[Pattern[str]] = []
    for value in values:
        try:
            patterns.append(re.compile(value))
        except re.error as error:
            raise RunnerError(f"invalid {option} regular expression {value!r}: {error}") from error
    return tuple(patterns)


def parse_extensions(value: str) -> frozenset[str]:
    extensions: set[str] = set()
    for item in value.split(","):
        item = item.strip().lower()
        if not item:
            continue
        if not item.startswith("."):
            item = "." + item
        extensions.add(item)
    if not extensions:
        raise RunnerError("--extensions must contain at least one extension")
    return frozenset(extensions)


def select_translation_units(
    units: Iterable[TranslationUnit],
    *,
    extensions: frozenset[str],
    include_patterns: Sequence[Pattern[str]],
    exclude_patterns: Sequence[Pattern[str]],
    include_missing: bool,
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
        selected.append(unit)

    sharded = [
        unit
        for index, unit in enumerate(selected)
        if index % shard_count == shard_index
    ]
    if max_files is not None:
        sharded = sharded[:max_files]
    return sharded, missing


def discover_tool(value: str | None) -> str:
    if value:
        candidate = pathlib.Path(value).expanduser()
        if candidate.parent != pathlib.Path(".") or candidate.is_absolute():
            if not candidate.is_file():
                raise RunnerError(f"ReturnGuard executable not found: {candidate}")
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


def build_command(
    *,
    tool: str,
    database_directory: pathlib.Path,
    source: pathlib.Path,
    mode: str,
    fail_on_diagnostics: bool,
    analyze_headers: bool,
    include_operators: bool,
    include_reference_returns: bool,
    no_color: bool,
    tool_arguments: Sequence[str],
) -> tuple[str, ...]:
    command = [tool, f"--mode={mode}"]
    if fail_on_diagnostics:
        command.append("--fail-on-diagnostics")
    if analyze_headers:
        command.append("--analyze-headers")
    if include_operators:
        command.append("--include-operators")
    if include_reference_returns:
        command.append("--include-reference-returns")
    if no_color:
        command.append("--no-color")
    command.extend(tool_arguments)
    command.extend(("-p", str(database_directory), str(source)))
    return tuple(command)


def run_translation_unit(
    unit: TranslationUnit,
    *,
    tool: str,
    database_directory: pathlib.Path,
    mode: str,
    fail_on_diagnostics: bool,
    analyze_headers: bool,
    include_operators: bool,
    include_reference_returns: bool,
    no_color: bool,
    tool_arguments: Sequence[str],
) -> RunResult:
    command = build_command(
        tool=tool,
        database_directory=database_directory,
        source=unit.source,
        mode=mode,
        fail_on_diagnostics=fail_on_diagnostics,
        analyze_headers=analyze_headers,
        include_operators=include_operators,
        include_reference_returns=include_reference_returns,
        no_color=no_color,
        tool_arguments=tool_arguments,
    )

    started = time.monotonic()
    completed = subprocess.run(
        command,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    return RunResult(
        source=unit.source,
        command=command,
        returncode=completed.returncode,
        output=completed.stdout,
        elapsed_seconds=time.monotonic() - started,
    )


def bounded_results(
    units: Sequence[TranslationUnit],
    *,
    jobs: int,
    worker,
    fail_fast: bool,
) -> Iterator[RunResult]:
    iterator = iter(units)
    stopped = False

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as executor:
        pending: dict[concurrent.futures.Future[RunResult], TranslationUnit] = {}

        def submit_next() -> bool:
            try:
                unit = next(iterator)
            except StopIteration:
                return False
            pending[executor.submit(worker, unit)] = unit
            return True

        for _ in range(min(jobs, len(units))):
            submit_next()

        while pending:
            done, _ = concurrent.futures.wait(
                pending,
                return_when=concurrent.futures.FIRST_COMPLETED,
            )
            for future in done:
                pending.pop(future)
                result = future.result()
                yield result
                if fail_fast and result.returncode != 0:
                    stopped = True

            if stopped:
                for future in pending:
                    future.cancel()
                continue

            for _ in done:
                submit_next()


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
    result.add_argument("--max-files", type=int)
    result.add_argument("--fail-fast", action="store_true")
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


def main(argv: Sequence[str] | None = None) -> int:
    arguments = parser().parse_args(argv)

    try:
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

        database = compile_database_path(arguments.build_path)
        all_units = load_translation_units(database)
        selected, missing = select_translation_units(
            all_units,
            extensions=parse_extensions(arguments.extensions),
            include_patterns=compile_patterns(
                arguments.include_regex, "--include-regex"
            ),
            exclude_patterns=compile_patterns(
                arguments.exclude_regex, "--exclude-regex"
            ),
            include_missing=arguments.include_missing,
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
        worker_arguments = dict(
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
                    " ".join(
                        build_command(source=unit.source, **worker_arguments)
                    )
                )
            return 0

        started = time.monotonic()
        completed_count = 0
        failed_count = 0

        def worker(unit: TranslationUnit) -> RunResult:
            return run_translation_unit(unit, **worker_arguments)

        for result in bounded_results(
            selected,
            jobs=min(jobs, len(selected)),
            worker=worker,
            fail_fast=arguments.fail_fast,
        ):
            completed_count += 1
            if result.returncode != 0:
                failed_count += 1

            if arguments.verbose:
                print(
                    f"returnguard-project: [{completed_count}/{len(selected)}] "
                    f"{result.source} ({result.elapsed_seconds:.2f}s, "
                    f"exit {result.returncode})",
                    file=sys.stderr,
                )
                print(
                    "returnguard-project: command: " + " ".join(result.command),
                    file=sys.stderr,
                )

            if result.output:
                sys.stdout.write(result.output)
                if not result.output.endswith("\n"):
                    sys.stdout.write("\n")
                sys.stdout.flush()

            if (
                arguments.progress_every > 0
                and completed_count % arguments.progress_every == 0
            ):
                print(
                    f"returnguard-project: completed {completed_count}/"
                    f"{len(selected)} translation units",
                    file=sys.stderr,
                )

        elapsed = time.monotonic() - started
        print(
            "returnguard-project: "
            f"analyzed {completed_count}/{len(selected)} translation units "
            f"in {elapsed:.2f}s with {min(jobs, len(selected))} job(s); "
            f"{failed_count} failed; {missing} missing source file(s) skipped",
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
