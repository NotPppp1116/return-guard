#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import pathlib
import subprocess
import sys
import tempfile


def run(
    command: list[str], *, environment: dict[str, str] | None = None
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=environment,
        check=False,
    )


def fail(message: str, output: str = "") -> int:
    print(f"read-only launcher test: {message}", file=sys.stderr)
    if output:
        print(output, file=sys.stderr)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--launcher", required=True)
    parser.add_argument("--tool", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--include", required=True)
    parser.add_argument("--runtime", required=True)
    arguments = parser.parse_args()

    launcher = pathlib.Path(arguments.launcher).resolve()
    tool = pathlib.Path(arguments.tool).resolve()
    compiler = pathlib.Path(arguments.compiler).resolve()
    include = pathlib.Path(arguments.include).resolve()
    runtime = pathlib.Path(arguments.runtime).resolve()

    with tempfile.TemporaryDirectory(prefix="returnguard-readonly-") as name:
        directory = pathlib.Path(name)
        source_directory = directory / "source"
        build_directory = directory / "build"
        source_directory.mkdir()
        build_directory.mkdir()

        header = source_directory / "local.h"
        source = source_directory / "main.c"
        object_file = build_directory / "main.o"
        dependency_file = build_directory / "main.d"
        program = build_directory / "program"

        header.write_text("#define LOCAL_VALUE 7\n", encoding="utf-8")
        source.write_text(
            """#include \"local.h\"
#include <returnguard/Contracts.h>

static int unavailable(void) RETURNGUARD_FAILS_NEGATIVE;
static int unavailable(void) { return -1; }
static int consume(int value) { return value + LOCAL_VALUE; }
int main(void) { return consume(unavailable()); }
""",
            encoding="utf-8",
        )

        environment = dict(os.environ)
        environment["RETURNGUARD_TOOL"] = str(tool)
        environment["RETURNGUARD_INCLUDE_DIR"] = str(include)

        os.chmod(header, 0o444)
        os.chmod(source, 0o444)
        os.chmod(source_directory, 0o555)
        try:
            compilation = run(
                [
                    sys.executable,
                    str(launcher),
                    str(compiler),
                    "-std=c17",
                    "-Wall",
                    "-Wextra",
                    "-Wpedantic",
                    "-Werror",
                    "-MMD",
                    "-MF",
                    str(dependency_file),
                    "-I",
                    str(include),
                    "-c",
                    str(source),
                    "-o",
                    str(object_file),
                ],
                environment=environment,
            )
        finally:
            os.chmod(source_directory, 0o755)
            os.chmod(source, 0o644)
            os.chmod(header, 0o644)

        if compilation.returncode != 0:
            return fail("instrumented compilation failed", compilation.stdout)
        if not object_file.is_file():
            return fail("compiler launcher produced no object file")
        if list(source_directory.glob(".*.returnguard-*")):
            return fail("compiler launcher wrote a temporary file into the source tree")
        if list(build_directory.glob(".returnguard-*")):
            return fail("compiler launcher left its temporary build directory behind")

        dependency_text = dependency_file.read_text(
            encoding="utf-8", errors="surrogateescape"
        )
        if str(source) not in dependency_text or ".returnguard-" in dependency_text:
            return fail("dependency output did not preserve the original source", dependency_text)
        if str(header) not in dependency_text:
            return fail("quoted include resolution did not preserve the local header", dependency_text)

        linking = run(
            [str(compiler), str(object_file), str(runtime), "-o", str(program)]
        )
        if linking.returncode != 0:
            return fail("linking failed", linking.stdout)

        execution = run([str(program)])
        if execution.returncode != 127:
            return fail(
                f"instrumented program exited with {execution.returncode}, expected 127",
                execution.stdout,
            )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
