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
    print(f"CMake package test: {message}", file=sys.stderr)
    if output:
        print(output, file=sys.stderr)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--compiler", required=True)
    arguments = parser.parse_args()

    cmake = pathlib.Path(arguments.cmake).resolve()
    returnguard_build = pathlib.Path(arguments.build_dir).resolve()
    compiler = pathlib.Path(arguments.compiler).resolve()

    with tempfile.TemporaryDirectory(prefix="returnguard-package-") as directory_name:
        directory = pathlib.Path(directory_name)
        prefix = directory / "prefix"
        source_dir = directory / "sample"
        build_dir = directory / "sample-build"
        source_dir.mkdir()

        install = run(
            [
                str(cmake),
                "--install",
                str(returnguard_build),
                "--prefix",
                str(prefix),
            ]
        )
        if install.returncode != 0:
            return fail("installing ReturnGuard failed", install.stdout)

        (source_dir / "CMakeLists.txt").write_text(
            """cmake_minimum_required(VERSION 3.20)
project(returnguard_package_consumer LANGUAGES C)
find_package(ReturnGuard CONFIG REQUIRED)
add_executable(hardened main.c)
returnguard_harden_target(hardened)
""",
            encoding="utf-8",
        )
        (source_dir / "main.c").write_text(
            """#include <returnguard/Contracts.h>
static int unavailable(void) RETURNGUARD_FAILS_NEGATIVE;
static int unavailable(void) { return -1; }
int main(void) { return unavailable(); }
""",
            encoding="utf-8",
        )

        configure = run(
            [
                str(cmake),
                "-S",
                str(source_dir),
                "-B",
                str(build_dir),
                f"-DCMAKE_PREFIX_PATH={prefix}",
                f"-DCMAKE_C_COMPILER={compiler}",
            ]
        )
        if configure.returncode != 0:
            return fail("consumer configuration failed", configure.stdout)

        build = run([str(cmake), "--build", str(build_dir)])
        if build.returncode != 0:
            return fail("consumer hardened build failed", build.stdout)

        program = build_dir / "hardened"
        if not program.is_file():
            return fail("consumer build did not produce the hardened executable")

        execution = run([str(program)])
        if execution.returncode != 127:
            return fail(
                f"installed-package program exited with {execution.returncode}, expected 127",
                execution.stdout,
            )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
