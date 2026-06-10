#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import tempfile


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", required=True)
    parser.add_argument("--compiler", required=True)
    return parser.parse_args()


def run(command: list[str], cwd: pathlib.Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def require_success(result: subprocess.CompletedProcess[str], description: str) -> None:
    output = result.stdout
    if result.returncode != 0:
        raise AssertionError(
            f"{description} exited with {result.returncode}:\n{output}"
        )
    if "fatal error:" in output or "file not found" in output:
        raise AssertionError(f"{description} failed to resolve headers:\n{output}")
    if output.count("warning: returnguard:") != 1:
        raise AssertionError(
            f"{description} did not produce exactly one ReturnGuard warning:\n{output}"
        )


def main() -> int:
    args = parse_args()
    tool = str(pathlib.Path(args.tool).resolve())
    compiler = str(pathlib.Path(args.compiler).resolve())

    with tempfile.TemporaryDirectory(prefix="returnguard-auto-") as temporary:
        project = pathlib.Path(temporary)
        source_directory = project / "src"
        header_directory = project / "header"
        build_directory = project / "build"
        source_directory.mkdir()
        header_directory.mkdir()
        build_directory.mkdir()

        source = source_directory / "main.c"
        source.write_text(
            '#include "layout.h"\n'
            "#include <stddef.h>\n\n"
            "int main(void) {\n"
            "    layout_status();\n"
            "    return (int)sizeof(size_t);\n"
            "}\n",
            encoding="utf-8",
        )
        (header_directory / "layout.h").write_text(
            "#pragma once\n\nint layout_status(void);\n",
            encoding="utf-8",
        )

        database = [
            {
                "directory": str(project),
                "arguments": [
                    compiler,
                    "-std=c17",
                    f"-I{header_directory}",
                    "-c",
                    str(source),
                ],
                "file": str(source),
            }
        ]
        (build_directory / "compile_commands.json").write_text(
            json.dumps(database),
            encoding="utf-8",
        )

        automatic = run([tool, "--no-color", str(source)], project)
        require_success(automatic, "automatic compilation database discovery")

        fixed = run(
            [
                tool,
                "--no-color",
                str(source),
                "--",
                "-std=c17",
                f"-I{header_directory}",
            ],
            project,
        )
        require_success(fixed, "automatic Clang resource directory")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
