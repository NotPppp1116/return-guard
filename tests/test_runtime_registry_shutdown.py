#!/usr/bin/env python3

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys
import tempfile


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def fail(message: str, output: str = "") -> int:
    print(f"runtime registry shutdown test: {message}", file=sys.stderr)
    if output:
        print(output, file=sys.stderr)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True)
    parser.add_argument("--include", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--runtime", required=True)
    arguments = parser.parse_args()

    source = pathlib.Path(arguments.source).resolve()
    include = pathlib.Path(arguments.include).resolve()
    compiler = pathlib.Path(arguments.compiler).resolve()
    runtime = pathlib.Path(arguments.runtime).resolve()

    with tempfile.TemporaryDirectory(prefix="returnguard-runtime-shutdown-") as name:
        program = pathlib.Path(name) / "runtime-shutdown"
        compilation = run(
            [
                str(compiler),
                "-std=c17",
                "-Wall",
                "-Wextra",
                "-Wpedantic",
                "-Werror",
                "-I",
                str(include),
                str(source),
                str(runtime),
                "-o",
                str(program),
            ]
        )
        if compilation.returncode != 0:
            return fail("compilation failed", compilation.stdout)

        execution = run([str(program)])
        if execution.returncode != 127:
            return fail(
                f"runtime exited with {execution.returncode}, expected 127",
                execution.stdout,
            )
        if "registry-closed" not in execution.stdout or "registry-open" in execution.stdout:
            return fail("fatal processing did not close registry updates", execution.stdout)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
