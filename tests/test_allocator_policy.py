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
    print(f"allocator policy test: {message}", file=sys.stderr)
    if output:
        print(output, file=sys.stderr)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--include", required=True)
    parser.add_argument("--compiler", required=True)
    arguments = parser.parse_args()

    tool = pathlib.Path(arguments.tool).resolve()
    source = pathlib.Path(arguments.source).resolve()
    include = pathlib.Path(arguments.include).resolve()
    compiler = pathlib.Path(arguments.compiler).resolve()

    with tempfile.TemporaryDirectory(prefix="returnguard-allocator-policy-") as name:
        directory = pathlib.Path(name)
        transformed = directory / "instrumented.c"
        transformation = run(
            [
                str(tool),
                "--no-color",
                f"--instrument-output={transformed}",
                str(source),
                "--",
                "-std=c17",
                "-I",
                str(include),
            ]
        )
        if transformation.returncode != 0:
            return fail("transformation failed", transformation.stdout)
        if not transformed.is_file():
            return fail("transformation produced no output")

        text = transformed.read_text(encoding="utf-8")
        if "__RG_CHECK_" in text:
            return fail(
                "a zero-size-capable allocator received an unconditional check",
                text,
            )
        if "#include <returnguard/Runtime.h>" in text:
            return fail("runtime header was injected without a rewritten call", text)

        object_file = directory / "instrumented.o"
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
                "-c",
                str(transformed),
                "-o",
                str(object_file),
            ]
        )
        if compilation.returncode != 0:
            return fail("transformed source did not compile", compilation.stdout)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
