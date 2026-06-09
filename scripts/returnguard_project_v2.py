#!/usr/bin/env python3

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from typing import Callable, Iterable, Iterator, Pattern, Sequence


DEFAULT_EXTENSIONS = (".c",)
DEFAULT_JOB_LIMIT = 8


class RunnerError(RuntimeError):
    pass


@dataclass(frozen=True)
class TranslationUnit:
    source: pathlib.Path


@dataclass(frozen=True)
class RunResult:
    source: pathlib.Path
    command: tuple[str, ...]
    returncode: int
    output_path: pathlib.Path
    elapsed_seconds: float
    timed_out: bool = False


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

    selected = [
        unit
        for index, unit in enumerate(selected)
        if index % shard_count == shard_index
    ]
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
                raise RunnerError(f"ReturnGuard executable is not executable: {candidate}")
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


def output_path_for(directory: pathlib.Path, source: pathlib.Path) -> pathlib.Path:
    digest = hashlib.sha256(str(source).encode("utf-8")).hexdigest()[:20]
    return directory / f"{digest}.log"


def run_translation_unit(
    unit: TranslationUnit,
    *,
    output_directory: pathlib.Path,
    timeout_seconds: float | None,
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
    output_path = output_path_for(output_directory, unit.source)
    started = time.monotonic()
    returncode = 127
    timed_out = False

    with output_path.open("w", encoding="utf-8", errors="replace") as output:
        try:
            completed = subprocess.run(
                command,
                text=True,
                stdout=output,
                stderr=subprocess.STDOUT,
                encoding="utf-8",
                errors="replace",
                timeout=timeout_seconds,
                check=False,
            )
            returncode = completed.returncode
        except subprocess.TimeoutExpired:
            timed_out = True
            returncode = 124
            output.write(
                f"returnguard-project: timed out after {timeout_seconds:g}s: "
                f"{unit.source}\n"
            )
        except OSError as error:
            output.write(
                f"returnguard-project: failed to start ReturnGuard for "
                f"{unit.source}: {error}\n"
            )

    return RunResult(
        source=unit.source,
        command=command,
        returncode=returncode,
        output_path=output_path,
        elapsed_seconds=time.monotonic() - started,
        timed_out=timed_out,
    )


def bounded_results(
    units: Sequence[TranslationUnit],
    *,
    jobs: int,
    worker: Callable[[TranslationUnit], RunResult],
    fail_fast: bool,
) -> Iterator[RunResult]:
    iterator = iter(units)
    stop_submitting = False

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as executor:
        pending: set[concurrent.futures.Future[RunResult]] = set()

        def submit_next() -> bool:
            try:
                unit = next(iterator)
            except StopIteration:
                return False
            pending.add(executor.submit(worker, unit))
            return True

        for _ in range(min(jobs, len(units))):
            submit_next()

        while pending:
            done, pending = concurrent.futures.wait(
                pending,
                return_when=concurrent.futures.FIRST_COMPLETED,
            )

            completed_slots = 0
            for future in done:
                if future.cancelled():
                    continue
                result = future.result()
                completed_slots += 1
                yield result
                if fail_fast and result.returncode != 0:
                    stop_submitting = True

            if stop_submitting:
                for future in pending:
                    future.cancel()
                continue

            for _ in range(completed_slots):
                submit_next()


def copy_or_print_output(
    result: RunResult,
    *,
    log_directory: pathlib.Path | None,
    stream_output: bool,
) -> None:
    if log_directory is not None:
        log_directory.mkdir(parents=True, exist_ok=True)
        destination = log_directory / (
            result.output_path.stem + "-" + result.source.name + ".log"
        )
        shutil.copyfile(result.output_path, destination)

    if not stream_output:
        return

    with result.output_path.open("r", encoding="utf-8", errors="replace") as output:
        shutil.copyfileobj(output, sys.stdout)
    sys.stdout.flush()


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
            print("returnguard-project: no matching translation units", file=sys.stderr)
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
                print(shlex.join(build_command(source=unit.source, **common_arguments)))
            return 0

        started = time.monotonic()
        completed_count = 0
        failed_count = 0
        timed_out_count = 0

        with tempfile.TemporaryDirectory(prefix="returnguard-project-") as temporary:
            output_directory = pathlib.Path(temporary)

            def worker(unit: TranslationUnit) -> RunResult:
                return run_translation_unit(
                    unit,
                    output_directory=output_directory,
                    timeout_seconds=arguments.timeout,
                    **common_arguments,
                )

            for run_result in bounded_results(
                selected,
                jobs=min(jobs, len(selected)),
                worker=worker,
                fail_fast=arguments.fail_fast,
            ):
                completed_count += 1
                if run_result.returncode != 0:
                    failed_count += 1
                if run_result.timed_out:
                    timed_out_count += 1

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

        elapsed = time.monotonic() - started
        stopped_early = completed_count < len(selected)
        print(
            "returnguard-project: "
            f"analyzed {completed_count}/{len(selected)} translation units "
            f"in {elapsed:.2f}s with {min(jobs, len(selected))} job(s); "
            f"{failed_count} failed; {timed_out_count} timed out; "
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
