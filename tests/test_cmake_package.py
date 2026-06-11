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
            """#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include <returnguard/Contracts.h>
#include <returnguard/Runtime.h>

static unsigned char secret[] = {1U, 2U, 3U, 4U};

static int unavailable(void) RETURNGUARD_FAILS_NEGATIVE;
static int unavailable(void) { return -1; }
static int consume(int value) { return value; }

void __rg_fatal_hook(uint32_t site_id, int saved_errno) {
    (void)site_id;
    (void)saved_errno;
    const char wiped[] = "secret-wiped\\n";
    const char not_wiped[] = "secret-not-wiped\\n";
    const int was_wiped =
        secret[0] == 0U && secret[1] == 0U &&
        secret[2] == 0U && secret[3] == 0U;
    if (was_wiped != 0) {
        (void)write(2, wiped, sizeof(wiped) - 1U);
    } else {
        (void)write(2, not_wiped, sizeof(not_wiped) - 1U);
    }
}

int main(void) {
    if (returnguard_register_secret(secret, sizeof(secret)) !=
        RETURNGUARD_SECRET_OK) {
        return 10;
    }
    if (returnguard_register_secret(secret, sizeof(secret)) !=
        RETURNGUARD_SECRET_ALREADY_REGISTERED) {
        return 11;
    }
    return consume(unavailable());
}
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
        if "secret-wiped" not in execution.stdout or "secret-not-wiped" in execution.stdout:
            return fail("registered secret was not wiped before the hook", execution.stdout)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
