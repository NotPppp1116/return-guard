#!/usr/bin/env python3

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys
import tempfile


def fail(message: str, *, output: str = "") -> int:
    print(f"instrumentation test: {message}", file=sys.stderr)
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

    source = pathlib.Path(arguments.source).resolve()
    include = pathlib.Path(arguments.include).resolve()

    with tempfile.TemporaryDirectory(prefix="returnguard-instrumentation-") as directory:
        output_source = pathlib.Path(directory) / "instrumented.c"
        output_object = pathlib.Path(directory) / "instrumented.o"

        transform = subprocess.run(
            [
                arguments.tool,
                "--no-color",
                f"--instrument-output={output_source}",
                str(source),
                "--",
                "-std=c17",
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if transform.returncode != 0:
            return fail("ReturnGuard transformation failed", output=transform.stdout)
        if not output_source.is_file():
            return fail("ReturnGuard did not create the transformed source")

        transformed = output_source.read_text(encoding="utf-8")
        if "#include <returnguard/Runtime.h>" not in transformed:
            return fail("runtime header was not injected", output=transformed)
        if transformed.count("__RG_CHECK_NULL(") != 1:
            return fail("expected exactly one null-result wrapper", output=transformed)
        if transformed.count("__RG_CHECK_NEGATIVE(") != 2:
            return fail("expected exactly two negative-result wrappers", output=transformed)
        if "int handled = open(path, O_RDONLY);" not in transformed:
            return fail("an already handled open call was rewritten", output=transformed)
        if "consume_fd(__RG_CHECK_NEGATIVE(open(path, O_RDONLY)" not in transformed:
            return fail("nested unchecked open call was not rewritten", output=transformed)

        compile_result = subprocess.run(
            [
                arguments.compiler,
                "-std=c17",
                "-I",
                str(include),
                "-c",
                str(output_source),
                "-o",
                str(output_object),
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if compile_result.returncode != 0:
            return fail("transformed source did not compile", output=compile_result.stdout)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
