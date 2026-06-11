#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
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
            "compiler response files are not supported yet; refusing to bypass instrumentation"
        )

    language = explicit_language(arguments)
    language_selects_source = language in SOURCE_LANGUAGES
    candidates: list[tuple[int, pathlib.Path]] = []
    for index, argument in positional_arguments(arguments):
        if argument == "-" and language_selects_source:
            raise LauncherError(
                "source from standard input is not supported by the hardened launcher"
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


def dependency_file(arguments: Sequence[str]) -> pathlib.Path | None:
    for index, argument in enumerate(arguments):
        if argument == "-MF" and index + 1 < len(arguments):
            return pathlib.Path(arguments[index + 1])
        if argument.startswith("-MF") and len(argument) > 3:
            return pathlib.Path(argument[3:])
    return None


def compiler_output_file(arguments: Sequence[str]) -> pathlib.Path | None:
    for index, argument in enumerate(arguments):
        if argument == "-o" and index + 1 < len(arguments):
            return pathlib.Path(arguments[index + 1])
        if (
            argument.startswith("-o")
            and len(argument) > 2
            and argument not in {"-ObjC", "-ObjC++"}
        ):
            return pathlib.Path(argument[2:])
    return None


def site_map_output(
    arguments: Sequence[str], original_source: pathlib.Path
) -> pathlib.Path | None:
    configured = os.environ.get("RETURNGUARD_SITE_MAP_DIR")
    if not configured:
        return None

    object_file = compiler_output_file(arguments)
    if object_file is None:
        raise LauncherError(
            "RETURNGUARD_SITE_MAP_DIR requires a compile command with an explicit -o output"
        )
    if not object_file.is_absolute():
        object_file = pathlib.Path.cwd() / object_file
    object_file = object_file.resolve(strict=False)

    directory = pathlib.Path(configured).expanduser().resolve(strict=False)
    directory.mkdir(parents=True, exist_ok=True)
    identity = f"{object_file}\0{original_source}".encode("utf-8", errors="surrogateescape")
    digest = hashlib.sha256(identity).hexdigest()[:16]
    return directory / f"{object_file.name}.{digest}.json"


def insert_compiler_options(
    arguments: Sequence[str], additional: Sequence[str]
) -> list[str]:
    result = list(arguments)
    try:
        separator = result.index("--")
    except ValueError:
        separator = len(result)
    result[separator:separator] = additional
    return result


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
        returnguard = executable_from_environment("RETURNGUARD_TOOL", "returnguard")
        include = runtime_include_directory()
        metadata_output = site_map_output(arguments, original_source)
        if metadata_output is not None:
            metadata_output.unlink(missing_ok=True)

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
            transform_command = [
                str(returnguard),
                "--no-color",
                f"--instrument-output={temporary_source}",
            ]
            if metadata_output is not None:
                transform_command.append(f"--site-map-output={metadata_output}")
                source_root = os.environ.get("RETURNGUARD_SOURCE_ROOT")
                if source_root:
                    transform_command.append(
                        f"--site-root={pathlib.Path(source_root).expanduser().resolve(strict=False)}"
                    )
            transform_command.extend(
                [
                    str(original_source),
                    "--",
                    *analysis_arguments,
                ]
            )

            transform = subprocess.run(transform_command, check=False)
            if transform.returncode != 0:
                if metadata_output is not None:
                    metadata_output.unlink(missing_ok=True)
                return transform.returncode
            if not temporary_source.is_file() or temporary_source.stat().st_size == 0:
                raise LauncherError("ReturnGuard produced no transformed source")
            if metadata_output is not None and not metadata_output.is_file():
                raise LauncherError("ReturnGuard produced no site metadata output")

            add_original_line_mapping(temporary_source, original_source)

            compiler_arguments = list(arguments)
            compiler_arguments[source_index] = str(temporary_source)
            path_options = [
                "-I",
                str(include),
                f"-ffile-prefix-map={temporary_source}={original_source}",
                f"-fdebug-prefix-map={temporary_source}={original_source}",
                f"-fmacro-prefix-map={temporary_source}={original_source}",
            ]
            compiler_arguments = insert_compiler_options(
                compiler_arguments, path_options
            )
            result = subprocess.run(
                [compiler, *compiler_arguments],
                check=False,
            )
            rewrite_dependency_file(
                dependency_file(arguments), temporary_source, original_source
            )
            if result.returncode != 0 and metadata_output is not None:
                metadata_output.unlink(missing_ok=True)
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
