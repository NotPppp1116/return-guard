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
    print(f"contract policy test: {message}", file=sys.stderr)
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

    base = run(
        [
            str(tool),
            "--no-color",
            str(source),
            "--",
            "-std=c++20",
            "-I",
            str(include),
        ]
    )
    if base.returncode != 0:
        return fail("base contract analysis failed", base.stdout)
    for expected in (
        "return value of 'openat' is consumed but not verified",
        "return value of 'close' is consumed but not verified",
        "return value of 'fputs' is consumed but not verified",
    ):
        if expected not in base.stdout:
            return fail(f"missing built-in contract diagnostic: {expected}", base.stdout)
    if "return value of 'vendor::open'" in base.stdout:
        return fail("vendor::open inherited a global POSIX contract", base.stdout)

    with tempfile.TemporaryDirectory(prefix="returnguard-contract-policy-") as name:
        directory = pathlib.Path(name)
        custom = run(
            [
                str(tool),
                "--no-color",
                "--contract=vendor::open=negative",
                str(source),
                "--",
                "-std=c++20",
                "-I",
                str(include),
            ]
        )
        if custom.returncode != 0:
            return fail("custom contract analysis failed", custom.stdout)
        if "return value of 'vendor::open' is consumed but not verified" not in custom.stdout:
            return fail("custom CLI contract did not diagnose vendor::open", custom.stdout)

        custom_null = run(
            [
                str(tool),
                "--no-color",
                "--contract=null_factory=null",
                str(source),
                "--",
                "-std=c++20",
                "-I",
                str(include),
            ]
        )
        if custom_null.returncode != 0:
            return fail("custom null contract analysis failed", custom_null.stdout)
        if (
            "potentially-null return value of 'null_factory' is dereferenced without a prior null check"
            not in custom_null.stdout
        ):
            return fail("custom null contract did not drive null-state analysis", custom_null.stdout)

        contract_file = directory / "contracts.txt"
        contract_file.write_text("# project contracts\nvendor::open=negative\n", encoding="utf-8")
        from_file = run(
            [
                str(tool),
                "--no-color",
                f"--contract-file={contract_file}",
                str(source),
                "--",
                "-std=c++20",
                "-I",
                str(include),
            ]
        )
        if from_file.returncode != 0:
            return fail("contract-file analysis failed", from_file.stdout)
        if "return value of 'vendor::open' is consumed but not verified" not in from_file.stdout:
            return fail("contract file did not diagnose vendor::open", from_file.stdout)

        function_config = directory / "function-config.rg"
        function_config.write_text(
            "# combined function policy\ncontract vendor::open negative\n",
            encoding="utf-8",
        )
        from_config = run(
            [
                str(tool),
                "--no-color",
                f"--function-config={function_config}",
                str(source),
                "--",
                "-std=c++20",
                "-I",
                str(include),
            ]
        )
        if from_config.returncode != 0:
            return fail("function-config analysis failed", from_config.stdout)
        if "return value of 'vendor::open' is consumed but not verified" not in from_config.stdout:
            return fail("function config did not diagnose vendor::open", from_config.stdout)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
