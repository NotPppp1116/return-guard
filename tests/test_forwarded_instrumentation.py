#!/usr/bin/env python3

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--include", required=True)
    arguments = parser.parse_args()

    tool = pathlib.Path(arguments.tool).resolve()
    source = pathlib.Path(arguments.source).resolve()
    include = pathlib.Path(arguments.include).resolve()

    with tempfile.TemporaryDirectory(prefix="returnguard-forwarded-") as directory:
        output = pathlib.Path(directory) / "instrumented.c"
        result = subprocess.run(
            [
                str(tool),
                "--no-color",
                f"--instrument-output={output}",
                str(source),
                "--",
                "-std=c17",
                "-I",
                str(include),
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if result.returncode != 0:
            print("forwarded instrumentation test: transformation failed", file=sys.stderr)
            print(result.stdout, file=sys.stderr)
            return 1

        transformed = output.read_text(encoding="utf-8")
        if "__RG_CHECK_" in transformed:
            print(
                "forwarded instrumentation test: direct propagation was made fatal",
                file=sys.stderr,
            )
            print(transformed, file=sys.stderr)
            return 1
        if "#include <returnguard/Runtime.h>" in transformed:
            print(
                "forwarded instrumentation test: unused runtime header was injected",
                file=sys.stderr,
            )
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
