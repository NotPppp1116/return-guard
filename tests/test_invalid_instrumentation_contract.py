#!/usr/bin/env python3

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys
import tempfile


def run_transform(
    *, tool: pathlib.Path, source: pathlib.Path, output: pathlib.Path, include: pathlib.Path
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--include", required=True)
    arguments = parser.parse_args()

    tool = pathlib.Path(arguments.tool).resolve()
    source = pathlib.Path(arguments.source).resolve()
    include = pathlib.Path(arguments.include).resolve()

    with tempfile.TemporaryDirectory(prefix="returnguard-contract-policy-") as directory:
        directory_path = pathlib.Path(directory)
        output = directory_path / "invalid-instrumented.c"
        result = run_transform(
            tool=tool, source=source, output=output, include=include
        )

        if result.returncode == 0:
            print(
                "invalid contract test: malformed contracts did not fail transformation",
                file=sys.stderr,
            )
            print(result.stdout, file=sys.stderr)
            return 1

        required = (
            "requires a signed integer return type",
            "requires a pointer return type",
        )
        for message in required:
            if message not in result.stdout:
                print(
                    f"invalid contract test: missing diagnostic {message!r}",
                    file=sys.stderr,
                )
                print(result.stdout, file=sys.stderr)
                return 1

        if output.is_file() and "__RG_CHECK_" in output.read_text(encoding="utf-8"):
            print(
                "invalid contract test: malformed contract was instrumented",
                file=sys.stderr,
            )
            return 1

        forwarded_source = source.parent / "instrumentation_forwarded.c"
        forwarded_output = directory_path / "forwarded-instrumented.c"
        forwarded = run_transform(
            tool=tool,
            source=forwarded_source,
            output=forwarded_output,
            include=include,
        )
        if forwarded.returncode != 0:
            print(
                "invalid contract test: forwarding-policy transformation failed",
                file=sys.stderr,
            )
            print(forwarded.stdout, file=sys.stderr)
            return 1

        forwarded_text = forwarded_output.read_text(encoding="utf-8")
        if "__RG_CHECK_" in forwarded_text:
            print(
                "invalid contract test: direct failure propagation was made fatal",
                file=sys.stderr,
            )
            print(forwarded_text, file=sys.stderr)
            return 1
        if "#include <returnguard/Runtime.h>" in forwarded_text:
            print(
                "invalid contract test: unused runtime header was injected for forwarding",
                file=sys.stderr,
            )
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
