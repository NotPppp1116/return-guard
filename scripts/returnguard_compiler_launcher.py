#!/usr/bin/env python3

from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import sys
from collections.abc import Sequence

SOURCE_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx", ".c++", ".C"})
SOURCE_LANGUAGES = frozenset(
    {
        "c",
        "c-header",
        "cpp-output",
        "c++",
        "c++-header",
        "c++-cpp-output",
    }
)
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
TOOL_ONLY_JOINED_PREFIXES = ("-MF", "-MT", "-MQ", "-MJ")


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
        if argument in OPTIONS_WITH_VALUE:
            index += 2
            continue
        if argument.startswith("-"):
            index += 1
            continue
        positional.append((index, argument))
        index += 1
    return positional


def explicit_language(arguments: Sequence[str]) -> str | None:
    language: str | None = None
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        if argument == "-x" and index + 1 < len(arguments):
            language = arguments[index + 1]
            index += 2
            continue
        if argument.startswith("-x") and len(argument) > 2:
            language = argument[2:]
        index += 1
    return language


def source_argument(arguments: Sequence[str]) -> tuple[int, pathlib.Path] | None:
    if any(argument.startswith("@") for argument in arguments):
        raise LauncherError(
            "compiler response files are not supported yet; refusing to bypass analysis"
        )

    language = explicit_language(arguments)
    language_selects_source = language in SOURCE_LANGUAGES
    candidates: list[tuple[int, pathlib.Path]] = []
    for index, argument in positional_arguments(arguments):
        if argument == "-" and language_selects_source:
            raise LauncherError(
                "source from standard input is not supported by the analysis launcher"
            )

        path = pathlib.Path(argument)
        if path.suffix not in SOURCE_SUFFIXES and not language_selects_source:
            continue
        if ".returnguard-" in path.name:
            continue

        absolute = path if path.is_absolute() else pathlib.Path.cwd() / path
        if not absolute.is_file():
            continue
        candidates.append((index, absolute.resolve(strict=False)))

    if not candidates:
        return None
    if len(candidates) != 1:
        raise LauncherError(
            "compiler launcher supports exactly one source file per compilation"
        )
    return candidates[0]


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
            argument.startswith(prefix) and len(argument) > len(prefix)
            for prefix in TOOL_ONLY_JOINED_PREFIXES
        ):
            index += 1
            continue
        filtered.append(argument)
        index += 1
    return filtered


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
        returnguard = executable_from_environment("RETURNGUARD_TOOL", "returnguard")
        analysis_arguments = tool_arguments(arguments, source_index)
        analysis_command = [
            str(returnguard),
            "--no-color",
            str(original_source),
            "--",
            *analysis_arguments,
        ]

        analysis = subprocess.run(analysis_command, check=False)
        if analysis.returncode != 0:
            return analysis.returncode

        return run_passthrough(compiler, arguments)
    except LauncherError as error:
        print(f"returnguard-compiler-launcher: {error}", file=sys.stderr)
        return 2
    except OSError as error:
        print(f"returnguard-compiler-launcher: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
