from __future__ import annotations

import concurrent.futures
import hashlib
import os
import pathlib
import signal
import shutil
import subprocess
import sys
import threading
import time
import traceback
from dataclasses import dataclass
from typing import Callable, Iterator, Sequence


PROCESS_POLL_SECONDS = 0.05
PROCESS_TERMINATE_GRACE_SECONDS = 1.0


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
    interrupted: bool = False
    internal_error: bool = False


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


def process_group_options() -> dict[str, object]:
    if os.name == "posix":
        return {"start_new_session": True}
    if os.name == "nt":
        return {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP}
    return {}


def signal_process_group(process: subprocess.Popen[str], sig: int) -> None:
    if process.poll() is not None:
        return

    try:
        if os.name == "posix":
            os.killpg(process.pid, sig)
        elif sig == getattr(signal, "SIGKILL", signal.SIGTERM):
            process.kill()
        else:
            process.terminate()
    except (OSError, ProcessLookupError):
        return


def terminate_process_group(
    process: subprocess.Popen[str],
    *,
    grace_seconds: float = PROCESS_TERMINATE_GRACE_SECONDS,
) -> None:
    if process.poll() is not None:
        return

    signal_process_group(process, signal.SIGTERM)
    try:
        process.wait(timeout=grace_seconds)
        return
    except subprocess.TimeoutExpired:
        pass

    signal_process_group(process, getattr(signal, "SIGKILL", signal.SIGTERM))
    try:
        process.wait(timeout=grace_seconds)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def wait_for_process(
    process: subprocess.Popen[str],
    *,
    stop_event: threading.Event,
    timeout_seconds: float | None,
) -> tuple[int, bool, bool]:
    deadline = (
        time.monotonic() + timeout_seconds
        if timeout_seconds is not None
        else None
    )

    while True:
        if stop_event.is_set():
            terminate_process_group(process)
            return 130, False, True

        wait_seconds = PROCESS_POLL_SECONDS
        if deadline is not None:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                terminate_process_group(process)
                return 124, True, False
            wait_seconds = min(wait_seconds, remaining)

        try:
            return process.wait(timeout=wait_seconds), False, False
        except subprocess.TimeoutExpired:
            continue


def run_translation_unit(
    unit: TranslationUnit,
    *,
    output_directory: pathlib.Path,
    timeout_seconds: float | None,
    stop_event: threading.Event,
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
    interrupted = False

    with output_path.open("w", encoding="utf-8", errors="replace") as output:
        try:
            process = subprocess.Popen(
                command,
                text=True,
                stdout=output,
                stderr=subprocess.STDOUT,
                encoding="utf-8",
                errors="replace",
                **process_group_options(),
            )
        except OSError as error:
            output.write(
                f"returnguard-project: failed to start ReturnGuard for "
                f"{unit.source}: {error}\n"
            )
        else:
            returncode, timed_out, interrupted = wait_for_process(
                process,
                stop_event=stop_event,
                timeout_seconds=timeout_seconds,
            )
            if timed_out:
                output.write(
                    f"returnguard-project: timed out after {timeout_seconds:g}s: "
                    f"{unit.source}\n"
                )
            elif interrupted:
                output.write(
                    f"returnguard-project: interrupted while analyzing "
                    f"{unit.source}\n"
                )

    return RunResult(
        source=unit.source,
        command=command,
        returncode=returncode,
        output_path=output_path,
        elapsed_seconds=time.monotonic() - started,
        timed_out=timed_out,
        interrupted=interrupted,
    )


def internal_error_result(
    unit: TranslationUnit,
    *,
    output_directory: pathlib.Path,
    command: tuple[str, ...],
) -> RunResult:
    output_path = output_path_for(output_directory, unit.source)
    try:
        output_path.write_text(
            "returnguard-project: internal worker failure while analyzing "
            f"{unit.source}\n{traceback.format_exc()}",
            encoding="utf-8",
            errors="replace",
        )
    except OSError:
        pass
    return RunResult(
        source=unit.source,
        command=command,
        returncode=125,
        output_path=output_path,
        elapsed_seconds=0.0,
        internal_error=True,
    )


def bounded_results(
    units: Sequence[TranslationUnit],
    *,
    jobs: int,
    worker: Callable[[TranslationUnit], RunResult],
    fail_fast: bool,
    stop_event: threading.Event,
) -> Iterator[RunResult]:
    iterator = iter(units)
    stop_submitting = False
    executor = concurrent.futures.ThreadPoolExecutor(max_workers=jobs)
    pending: set[concurrent.futures.Future[RunResult]] = set()

    def submit_next() -> bool:
        try:
            unit = next(iterator)
        except StopIteration:
            return False
        pending.add(executor.submit(worker, unit))
        return True

    try:
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
    except BaseException:
        stop_event.set()
        for future in pending:
            future.cancel()
        raise
    finally:
        if stop_event.is_set():
            for future in pending:
                future.cancel()
        executor.shutdown(wait=True, cancel_futures=True)


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
        try:
            shutil.copyfile(result.output_path, destination)
        except FileNotFoundError:
            destination.write_text(
                f"returnguard-project: output unavailable for {result.source}\n",
                encoding="utf-8",
            )

    if not stream_output:
        return

    try:
        with result.output_path.open(
            "r",
            encoding="utf-8",
            errors="replace",
        ) as output:
            shutil.copyfileobj(output, sys.stdout)
    except FileNotFoundError:
        print(
            f"returnguard-project: output unavailable for {result.source}",
            file=sys.stderr,
        )
    sys.stdout.flush()
