#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import pathlib
import subprocess
import sys
import tempfile


def fail(message: str, output: str = "") -> int:
    print(f"compiler launcher test: {message}", file=sys.stderr)
    if output:
        print(output, file=sys.stderr)
    return 1


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

    with tempfile.TemporaryDirectory(prefix="returnguard-launcher-") as directory_name:
        directory = pathlib.Path(directory_name)
        header = directory / "local.h"
        source = directory / "main.c"
        object_file = directory / "main.o"
        dependency_file = directory / "main.d"
        program = directory / "program"

        header.write_text("#define LOCAL_BIAS 2\n", encoding="utf-8")
        source.write_text(
            """#include \"local.h\"
#include <returnguard/Contracts.h>

static int unavailable_status(void) RETURNGUARD_FAILS_NEGATIVE;

static int unavailable_status(void) {
    return -1;
}

const char* compiled_source_name = __FILE__;

int main(void) {
    return unavailable_status() + LOCAL_BIAS;
}
""",
            encoding="utf-8",
        )

        environment = dict(os.environ)
        environment["RETURNGUARD_TOOL"] = str(tool)
        environment["RETURNGUARD_INCLUDE_DIR"] = str(include)

        compile_result = run(
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
        if compile_result.returncode != 0:
            return fail("instrumented compilation failed", compile_result.stdout)
        if not object_file.is_file():
            return fail("compiler launcher did not create the object file")

        leftovers = list(directory.glob(".*.returnguard-*"))
        if leftovers:
            return fail(f"temporary transformed source was not removed: {leftovers}")

        dependency_text = dependency_file.read_text(
            encoding="utf-8", errors="surrogateescape"
        )
        if ".returnguard-" in dependency_text:
            return fail(
                "dependency output still references the temporary source",
                dependency_text,
            )
        if str(source) not in dependency_text:
            return fail("dependency output does not reference the original source")

        object_bytes = object_file.read_bytes()
        if str(source).encode() not in object_bytes:
            return fail("#line mapping did not preserve the original __FILE__ path")
        if b".returnguard-" in object_bytes:
            return fail("temporary source path leaked into the object file")

        link_result = run(
            [str(compiler), str(object_file), str(runtime), "-o", str(program)]
        )
        if link_result.returncode != 0:
            return fail("linking the launcher-produced object failed", link_result.stdout)

        execution = run([str(program)])
        if execution.returncode != 127:
            return fail(
                f"instrumented program exited with {execution.returncode}, expected 127",
                execution.stdout,
            )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
