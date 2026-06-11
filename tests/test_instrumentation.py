#!/usr/bin/env python3

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys
import tempfile
from collections.abc import Sequence


def fail(message: str, *, output: str = "") -> int:
    print(f"instrumentation test: {message}", file=sys.stderr)
    if output:
        print(output, file=sys.stderr)
    return 1


def run(command: Sequence[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        list(command),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def transform(
    *,
    tool: pathlib.Path,
    source: pathlib.Path,
    output: pathlib.Path,
    include: pathlib.Path,
    standard: str,
) -> subprocess.CompletedProcess[str]:
    return run(
        [
            str(tool),
            "--no-color",
            f"--instrument-output={output}",
            str(source),
            "--",
            f"-std={standard}",
            "-I",
            str(include),
        ]
    )


def compile_program(
    *,
    compiler: pathlib.Path,
    source: pathlib.Path,
    output: pathlib.Path,
    include: pathlib.Path,
    runtime: pathlib.Path,
    standard: str,
) -> subprocess.CompletedProcess[str]:
    return run(
        [
            str(compiler),
            f"-std={standard}",
            "-Wall",
            "-Wextra",
            "-Wpedantic",
            "-Werror",
            "-Wconversion",
            "-Wsign-conversion",
            "-I",
            str(include),
            str(source),
            str(runtime),
            "-o",
            str(output),
        ]
    )


def require_transform(
    *,
    tool: pathlib.Path,
    source: pathlib.Path,
    output: pathlib.Path,
    include: pathlib.Path,
    standard: str,
) -> str:
    result = transform(
        tool=tool,
        source=source,
        output=output,
        include=include,
        standard=standard,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"ReturnGuard transformation failed for {source.name}\n{result.stdout}"
        )
    if not output.is_file():
        raise RuntimeError(f"ReturnGuard did not create output for {source.name}")
    return output.read_text(encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--cases", required=True)
    parser.add_argument("--include", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--cxx-compiler", required=True)
    parser.add_argument("--runtime", required=True)
    arguments = parser.parse_args()

    tool = pathlib.Path(arguments.tool).resolve()
    source = pathlib.Path(arguments.source).resolve()
    cases = pathlib.Path(arguments.cases).resolve()
    include = pathlib.Path(arguments.include).resolve()
    compiler = pathlib.Path(arguments.compiler).resolve()
    cxx_compiler = pathlib.Path(arguments.cxx_compiler).resolve()
    runtime = pathlib.Path(arguments.runtime).resolve()

    try:
        with tempfile.TemporaryDirectory(
            prefix="returnguard-instrumentation-"
        ) as directory_name:
            directory = pathlib.Path(directory_name)

            basic_output = directory / "instrumented-basic.c"
            basic = require_transform(
                tool=tool,
                source=source,
                output=basic_output,
                include=include,
                standard="c17",
            )
            if "#include <returnguard/Runtime.h>" not in basic:
                return fail("runtime header was not injected", output=basic)
            if basic.count("__RG_CHECK_NULL(") != 1:
                return fail("expected exactly one null-result wrapper", output=basic)
            if basic.count("__RG_CHECK_NEGATIVE(") != 2:
                return fail("expected exactly two negative-result wrappers", output=basic)
            if "int handled = open(path, O_RDONLY);" not in basic:
                return fail("an already handled open call was rewritten", output=basic)
            if "consume_fd(__RG_CHECK_NEGATIVE(open(path, O_RDONLY)" not in basic:
                return fail("nested unchecked open call was not rewritten", output=basic)

            basic_object = directory / "instrumented-basic.o"
            basic_compile = run(
                [
                    str(compiler),
                    "-std=c17",
                    "-Wall",
                    "-Wextra",
                    "-Wpedantic",
                    "-Werror",
                    "-Wconversion",
                    "-Wsign-conversion",
                    "-I",
                    str(include),
                    "-c",
                    str(basic_output),
                    "-o",
                    str(basic_object),
                ]
            )
            if basic_compile.returncode != 0:
                return fail(
                    "basic transformed source did not compile",
                    output=basic_compile.stdout,
                )

            contextual_source = cases / "instrumentation_contextual_io.c"
            contextual_output = directory / "instrumented-contextual-io.c"
            contextual = require_transform(
                tool=tool,
                source=contextual_source,
                output=contextual_output,
                include=include,
                standard="c17",
            )
            if "__RG_CHECK_" in contextual:
                return fail(
                    "context-sensitive read/write calls were made fatal by default",
                    output=contextual,
                )
            if "#include <returnguard/Runtime.h>" in contextual:
                return fail(
                    "runtime header was injected without any instrumented call",
                    output=contextual,
                )

            c_types_source = cases / "instrumentation_types.c"
            c_types_output = directory / "instrumented-types.c"
            c_types = require_transform(
                tool=tool,
                source=c_types_source,
                output=c_types_output,
                include=include,
                standard="c17",
            )
            if c_types.count("__RG_CHECK_NULL(") != 1:
                return fail("C custom null contract was not wrapped", output=c_types)
            if c_types.count("__RG_CHECK_NEGATIVE(") != 1:
                return fail("C custom negative contract was not wrapped", output=c_types)
            if "__RG_CHECK_NULL(make_packet()" not in c_types:
                return fail("C member-access call was not wrapped", output=c_types)

            c_types_program = directory / "instrumented-types"
            c_types_compile = compile_program(
                compiler=compiler,
                source=c_types_output,
                output=c_types_program,
                include=include,
                runtime=runtime,
                standard="c17",
            )
            if c_types_compile.returncode != 0:
                return fail(
                    "typed C transformed source did not compile",
                    output=c_types_compile.stdout,
                )
            c_types_run = run([str(c_types_program)])
            if c_types_run.returncode != 0:
                return fail(
                    "typed C wrapper changed value or evaluated a call twice",
                    output=c_types_run.stdout,
                )

            runtime_source = cases / "instrumentation_runtime.c"
            runtime_output = directory / "instrumented-runtime.c"
            runtime_text = require_transform(
                tool=tool,
                source=runtime_source,
                output=runtime_output,
                include=include,
                standard="c17",
            )
            if runtime_text.count("__RG_CHECK_NULL(") != 1:
                return fail("runtime failure call was not wrapped", output=runtime_text)

            runtime_program = directory / "instrumented-runtime"
            runtime_compile = compile_program(
                compiler=compiler,
                source=runtime_output,
                output=runtime_program,
                include=include,
                runtime=runtime,
                standard="c17",
            )
            if runtime_compile.returncode != 0:
                return fail(
                    "runtime failure test did not compile",
                    output=runtime_compile.stdout,
                )
            runtime_run = run([str(runtime_program)])
            if runtime_run.returncode != 127:
                return fail(
                    f"fatal runtime exited with {runtime_run.returncode}, expected 127",
                    output=runtime_run.stdout,
                )

            cpp_source = cases / "instrumentation_types.cpp"
            cpp_output = directory / "instrumented-types.cpp"
            cpp_text = require_transform(
                tool=tool,
                source=cpp_source,
                output=cpp_output,
                include=include,
                standard="c++20",
            )
            if cpp_text.count("__RG_CHECK_NULL(") != 1:
                return fail("C++ custom null contract was not wrapped", output=cpp_text)
            if cpp_text.count("__RG_CHECK_NEGATIVE(") != 1:
                return fail("C++ custom negative contract was not wrapped", output=cpp_text)

            cpp_program = directory / "instrumented-types-cpp"
            cpp_compile = compile_program(
                compiler=cxx_compiler,
                source=cpp_output,
                output=cpp_program,
                include=include,
                runtime=runtime,
                standard="c++20",
            )
            if cpp_compile.returncode != 0:
                return fail(
                    "typed C++ transformed source did not compile",
                    output=cpp_compile.stdout,
                )
            cpp_run = run([str(cpp_program)])
            if cpp_run.returncode != 0:
                return fail(
                    "typed C++ wrapper changed value or evaluated a call twice",
                    output=cpp_run.stdout,
                )
    except RuntimeError as error:
        return fail(str(error))

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
