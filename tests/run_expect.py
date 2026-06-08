#!/usr/bin/env python3

from __future__ import annotations

import argparse
import subprocess
import sys


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--include", required=True)
    parser.add_argument("--mode", required=True)
    parser.add_argument("--warning-count", type=int, required=True)
    parser.add_argument("--expected-exit-code", type=int, default=0)
    parser.add_argument(
        "--diagnostic-kind",
        choices=("warning", "error"),
        default="warning",
    )
    parser.add_argument("--fail-on-diagnostics", action="store_true")
    parser.add_argument("--expect", action="append", default=[])
    parser.add_argument("--reject", action="append", default=[])
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    command = [
        args.tool,
        f"--mode={args.mode}",
        "--no-color",
    ]
    if args.fail_on_diagnostics:
        command.append("--fail-on-diagnostics")

    command.extend(
        [
            args.source,
            "--",
            "-std=c17",
            f"-I{args.include}",
        ]
    )

    result = subprocess.run(
        command,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    output = result.stdout

    if result.returncode != args.expected_exit_code:
        print(output, end="")
        print(
            "expected ReturnGuard to exit with "
            f"{args.expected_exit_code}, got {result.returncode}; "
            f"command: {' '.join(command)}",
            file=sys.stderr,
        )
        return 1

    marker = f"{args.diagnostic_kind}: returnguard:"
    diagnostic_count = output.count(marker)
    if diagnostic_count != args.warning_count:
        print(output, end="")
        print(
            f"expected {args.warning_count} ReturnGuard {args.diagnostic_kind}(s), "
            f"got {diagnostic_count}",
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
