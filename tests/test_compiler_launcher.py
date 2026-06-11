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


def link_and_expect_fatal(
    *,
    compiler: pathlib.Path,
    object_file: pathlib.Path,
    runtime: pathlib.Path,
    program: pathlib.Path,
) -> tuple[bool, str]:
    link_result = run(
        [str(compiler), str(object_file), str(runtime), "-o", str(program)]
    )
    if link_result.returncode != 0:
        return False, f"linking failed\n{link_result.stdout}"

    execution = run([str(program)])
    if execution.returncode != 127:
        return (
            False,
            f"instrumented program exited with {execution.returncode}, expected 127\n"
            f"{execution.stdout}",
        )
    return True, ""


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

static int consume_status(int status) {
    return status + LOCAL_BIAS;
}

const char* compiled_source_name = __FILE__;

int main(void) {
    return consume_status(unavailable_status());
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
                "-g",
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
            return fail("original source path was not preserved in the object file")
        if b".returnguard-" in object_bytes:
            return fail("temporary source path leaked into debug or macro data")

        linked, message = link_and_expect_fatal(
            compiler=compiler,
            object_file=object_file,
            runtime=runtime,
            program=program,
        )
        if not linked:
            return fail("launcher-produced program failed", message)

        extensionless = directory / "extensionless"
        extensionless_object = directory / "extensionless.o"
        extensionless_program = directory / "extensionless-program"
        extensionless.write_text(
            """#include <returnguard/Contracts.h>
static int fail(void) RETURNGUARD_FAILS_NEGATIVE;
static int fail(void) { return -1; }
static int consume(int value) { return value; }
int main(void) { return consume(fail()); }
""",
            encoding="utf-8",
        )
        extensionless_compile = run(
            [
                sys.executable,
                str(launcher),
                str(compiler),
                "-x",
                "c",
                "-std=c17",
                "-I",
                str(include),
                "-c",
                str(extensionless),
                "-o",
                str(extensionless_object),
            ],
            environment=environment,
        )
        if extensionless_compile.returncode != 0:
            return fail(
                "extensionless -x c source was not instrumented",
                extensionless_compile.stdout,
            )
        linked, message = link_and_expect_fatal(
            compiler=compiler,
            object_file=extensionless_object,
            runtime=runtime,
            program=extensionless_program,
        )
        if not linked:
            return fail("extensionless launcher program failed", message)

        response_file = directory / "compile.rsp"
        response_object = directory / "response.o"
        response_file.write_text(
            f"-std=c17 -c {source} -o {response_object}\n", encoding="utf-8"
        )
        response_result = run(
            [
                sys.executable,
                str(launcher),
                str(compiler),
                f"@{response_file}",
            ],
            environment=environment,
        )
        if response_result.returncode != 2:
            return fail(
                "unsupported response-file compilation did not fail closed",
                response_result.stdout,
            )
        if "response files are not supported" not in response_result.stdout:
            return fail(
                "response-file failure did not explain the instrumentation limit",
                response_result.stdout,
            )
        if response_object.exists():
            return fail("response-file compilation bypassed instrumentation")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
