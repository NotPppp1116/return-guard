#!/usr/bin/env python3

from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Sequence

SOURCE_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx", ".c++", ".C"})
OPTIONS_WITH_VALUE = frozenset(
    {
        "-o",
        "-MF",
        "-MT",
        "-MQ",
        "-MJ",
        "-I",
        "-isystem",
        "-iquote",
        "-include",
        "-imacros",
        "-x",
        "--sysroot",
        "--target",
        "--serialize-diagnostics",
    }
)
TOOL_ONLY_DROPPED_OPTIONS = frozenset(
    {
        "-c",
        "-S",
        "-E",
        "-M",
        "-MM",
        "-MD",
        "-MMD",
        "-MG",
        "-MP",
    }
)
TOOL_ONLY_DROPPED_WITH_VALUE = frozenset(
    {"-o", "-MF", "-MT", "-MQ", "-MJ", "--serialize-diagnostics"}
)


class LauncherError(RuntimeError):
    pass


def executable_from_environment(name: str, fallback: str) -> pathlib.Path:
    configured = os.environ.get(name)
    candidate = configured if configured else shutil.which(fallback)
    if not candidate:
        raise LauncherError(
            f"could not locate {fallback!r}; set {name} to an explicit executable"
        )
    return pathlib.Path(candidate).expanduser().resolve()


def runtime_include_directory() -> pathlib.Path:
    configured = os.environ.get("RETURNGUARD_INCLUDE_DIR")
    if configured:
        include = pathlib.Path(configured).expanduser().resolve()
    else:
        launcher = pathlib.Path(__file__).resolve()
        include = launcher.parent.parent / "include"
        if not include.is_dir():
            include = launcher.parent.parent.parent / "include"

    header = include / "returnguard" / "Runtime.h"
    if not header.is_file():
        raise LauncherError(
            "could not locate returnguard/Runtime.h; set RETURNGUARD_INCLUDE_DIR"
        )
    return include


def option_consumes_next(argument: str) -> bool:
    return argument in OPTIONS_WITH_VALUE


def positional_arguments(arguments: Sequence[str]) -> list[tuple[int, str]]:
    positional: list[tuple[int, str]] = []
    index = 0
    after_separator = False
    while index < len(arguments):
        argument = arguments[index]
        if after_separator:
            positional.append((index, argument))
            index += 1
            continue
        if argument == "--":
            after_separator = True
            index += 1
            continue
        if option_consumes_next(argument):
            index += 2
            continue
        if argument.startswith("-"):
            index += 1
            continue
        positional.append((index, argument))
        index += 1
    return positional


def source_argument(arguments: Sequence[str]) -> tuple[int, pathlib.Path] | None:
    candidates: list[tuple[int, pathlib.Path]] = []
    for index, argument in positional_arguments(arguments):
        path = pathlib.Path(argument)
        if path.suffix not in SOURCE_SUFFIXES:
            continue
        if ".returnguard-" in path.name:
            continue
        candidates.append((index, path))

    if not candidates:
        return None
    if len(candidates) != 1:
        raise LauncherError(
            "compiler launcher supports exactly one source file per compilation"
        )

    index, path = candidates[0]
    if not path.is_absolute():
        path = pathlib.Path.cwd() / path
    return index, path.resolve(strict=False)


def tool_arguments(arguments: Sequence[str], source_index: int) -> list[str]:
    filtered: list[str] = []
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        if index == source_index:
            index += 1
            continue
        if argument in TOOL_ONLY_DROPPED_OPTIONS:
            index += 1
            continue
        if argument in TOOL_ONLY_DROPPED_WITH_VALUE:
            index += 2
            continue
        if any(
            argument.startswith(prefix)
            for prefix in ("-o", "-MF", "-MT", "-MQ", "-MJ")
        ) and argument not in {"-ObjC", "-ObjC++"}:
            index += 1
            continue
        filtered.append(argument)
        index += 1
    return filtered


def dependency_file(arguments: Sequence[str]) -> pathlib.Path | None:
    for index, argument in enumerate(arguments):
        if argument == "-MF" and index + 1 < len(arguments):
            return pathlib.Path(arguments[index + 1])
        if argument.startswith("-MF") and len(argument) > 3:
            return pathlib.Path(argument[3:])
    return None


def add_original_line_mapping(transformed: pathlib.Path, original: pathlib.Path) -> None:
    content = transformed.read_text(encoding="utf-8")
    include_line = "#include <returnguard/Runtime.h>\n"
    escaped = str(original).replace("\\", "\\\\").replace('"', '\\"')
    mapping = f'#line 1 "{escaped}"\n'
    if content.startswith(include_line):
        content = include_line + mapping + content[len(include_line) :]
    else:
        content = mapping + content
    transformed.write_text(content, encoding="utf-8")


def rewrite_dependency_file(
    path: pathlib.Path | None, temporary: pathlib.Path, original: pathlib.Path
) -> None:
    if path is None:
        return
    if not path.is_absolute():
        path = pathlib.Path.cwd() / path
    if not path.is_file():
        return

    content = path.read_text(encoding="utf-8", errors="surrogateescape")
    content = content.replace(str(temporary), str(original))
    path.write_text(content, encoding="utf-8", errors="surrogateescape")


def run_passthrough(compiler: str, arguments: Sequence[str]) -> int:
    return subprocess.run([compiler, *arguments], check=False).returncode


def main() -> int:
    if len(sys.argv) < 2:
        print(
            "usage: returnguard-compiler-launcher <compiler> [compiler arguments...]",
            file=sys.stderr,
        )
        return 2

    compiler = sys.argv[1]
    arguments = sys.argv[2:]

    if os.environ.get("RETURNGUARD_DISABLE") == "1":
        return run_passthrough(compiler, arguments)

    try:
        source = source_argument(arguments)
        if source is None:
            return run_passthrough(compiler, arguments)

        source_index, original_source = source
        if not original_source.is_file():
            raise LauncherError(f"source file does not exist: {original_source}")

        returnguard = executable_from_environment("RETURNGUARD_TOOL", "returnguard")
        include = runtime_include_directory()

        temporary_handle = tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            prefix=f".{original_source.stem}.returnguard-",
            suffix=original_source.suffix,
            dir=original_source.parent,
            delete=False,
        )
        temporary_source = pathlib.Path(temporary_handle.name)
        temporary_handle.close()

        try:
            analysis_arguments = tool_arguments(arguments, source_index)
            transform = subprocess.run(
                [
                    str(returnguard),
                    "--no-color",
                    f"--instrument-output={temporary_source}",
                    str(original_source),
                    "--",
                    *analysis_arguments,
                ],
                check=False,
            )
            if transform.returncode != 0:
                return transform.returncode

            add_original_line_mapping(temporary_source, original_source)

            compiler_arguments = list(arguments)
            compiler_arguments[source_index] = str(temporary_source)
            compiler_arguments.extend(["-I", str(include)])
            result = subprocess.run(
                [compiler, *compiler_arguments],
                check=False,
            )
            rewrite_dependency_file(
                dependency_file(arguments), temporary_source, original_source
            )
            return result.returncode
        finally:
            temporary_source.unlink(missing_ok=True)
    except LauncherError as error:
        print(f"returnguard-compiler-launcher: {error}", file=sys.stderr)
        return 2
    except OSError as error:
        print(f"returnguard-compiler-launcher: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
