#!/usr/bin/env python3

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--include", required=True)
    parser.add_argument("--mode", required=True)
    parser.add_argument("--warning-count", type=int, required=True)
    parser.add_argument("--expect", action="append", default=[])
    parser.add_argument("--reject", action="append", default=[])
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    command = [
        args.tool,
        f"--mode={args.mode}",
        args.source,
        "--",
        "-std=c17",
        f"-I{args.include}",
    ]

    result = subprocess.run(
        command,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    output = result.stdout

    if result.returncode != 0:
        print(output, end="")
        print(
            f"ReturnGuard exited with {result.returncode}; command: {' '.join(command)}",
            file=sys.stderr,
        )
        return 1

    warning_count = output.count("warning: returnguard:")
    if warning_count != args.warning_count:
        print(output, end="")
        print(
            f"expected {args.warning_count} ReturnGuard warning(s), got {warning_count}",
            file=sys.stderr,
        )
        return 1

    for expected in args.expect:
        if expected not in output:
            print(output, end="")
            print(f"missing expected diagnostic text: {expected!r}", file=sys.stderr)
            return 1

    for rejected in args.reject:
        if rejected in output:
            print(output, end="")
            print(f"unexpected diagnostic text: {rejected!r}", file=sys.stderr)
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
