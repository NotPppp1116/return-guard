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

    with tempfile.TemporaryDirectory(prefix="returnguard-contract-policy-") as name:
        directory = pathlib.Path(name)
        transformed = directory / "instrumented.cpp"
        result = run(
            [
                str(tool),
                "--no-color",
                f"--instrument-output={transformed}",
                str(source),
                "--",
                "-std=c++20",
                "-I",
                str(include),
            ]
        )
        if result.returncode != 0:
            return fail("transformation failed", result.stdout)
        if not transformed.is_file():
            return fail("transformation created no output")

        text = transformed.read_text(encoding="utf-8")
        if text.count("__RG_CHECK_NULL(") != 1:
            return fail("std::fopen did not receive exactly one null check", text)
        if "__RG_CHECK_NULL(std::fopen(" not in text:
            return fail("std::fopen was not recognized as a standard contract", text)
        if text.count("__RG_CHECK_NEGATIVE(") != 3:
            return fail("expanded negative-result contracts were not recognized", text)
        if "__RG_CHECK_NEGATIVE(vendor::open" in text:
            return fail("vendor::open inherited the global POSIX contract", text)
        if "__RG_CHECK_NEGATIVE(::openat(" not in text:
            return fail("openat was not recognized as a POSIX contract", text)
        if "__RG_CHECK_NEGATIVE(::close(" not in text:
            return fail("close was not recognized as a POSIX contract", text)
        if "__RG_CHECK_NEGATIVE(std::fputs(" not in text:
            return fail("std::fputs was not recognized as a standard contract", text)
        if "consume_status(vendor::open" not in text:
            return fail("vendor::open call was unexpectedly rewritten", text)

        custom_transformed = directory / "instrumented-custom.cpp"
        custom = run(
            [
                str(tool),
                "--no-color",
                "--contract=vendor::open=negative",
                f"--instrument-output={custom_transformed}",
                str(source),
                "--",
                "-std=c++20",
                "-I",
                str(include),
            ]
        )
        if custom.returncode != 0:
            return fail("custom contract transformation failed", custom.stdout)
        custom_text = custom_transformed.read_text(encoding="utf-8")
        if "__RG_CHECK_NEGATIVE(vendor::open" not in custom_text:
            return fail("custom CLI contract did not rewrite vendor::open", custom_text)

        contract_file = directory / "contracts.txt"
        contract_file.write_text("# project contracts\nvendor::open=negative\n", encoding="utf-8")
        file_transformed = directory / "instrumented-file.cpp"
        from_file = run(
            [
                str(tool),
                "--no-color",
                f"--contract-file={contract_file}",
                f"--instrument-output={file_transformed}",
                str(source),
                "--",
                "-std=c++20",
                "-I",
                str(include),
            ]
        )
        if from_file.returncode != 0:
            return fail("contract-file transformation failed", from_file.stdout)
        file_text = file_transformed.read_text(encoding="utf-8")
        if "__RG_CHECK_NEGATIVE(vendor::open" not in file_text:
            return fail("contract file did not rewrite vendor::open", file_text)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
