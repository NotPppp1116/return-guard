#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys
import tempfile
from typing import Any


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def fail(message: str, output: str = "") -> int:
    print(f"overload metadata test: {message}", file=sys.stderr)
    if output:
        print(output, file=sys.stderr)
    return 1


def fnv1a_64(text: str) -> int:
    value = 14695981039346656037
    for byte in text.encode("utf-8"):
        value ^= byte
        value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value if value != 0 else 1


def expected_identifier(site: dict[str, Any]) -> int:
    fields = (
        site["file"],
        str(site["line"]),
        str(site["column"]),
        site["function"],
        site["callee"],
        site["callee_type"],
        site["predicate"],
    )
    return fnv1a_64("\x1f".join(fields))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--include", required=True)
    parser.add_argument("--compiler", required=True)
    arguments = parser.parse_args()

    tool = pathlib.Path(arguments.tool).resolve()
    source = pathlib.Path(arguments.source).resolve()
    include = pathlib.Path(arguments.include).resolve()
    compiler = pathlib.Path(arguments.compiler).resolve()

    with tempfile.TemporaryDirectory(prefix="returnguard-overload-metadata-") as name:
        directory = pathlib.Path(name)
        transformed = directory / "instrumented.cpp"
        site_map = directory / "sites.json"

        transformation = run(
            [
                str(tool),
                "--no-color",
                f"--instrument-output={transformed}",
                f"--site-map-output={site_map}",
                f"--site-root={source.parent}",
                str(source),
                "--",
                "-std=c++20",
                "-I",
                str(include),
            ]
        )
        if transformation.returncode != 0:
            return fail("transformation failed", transformation.stdout)

        try:
            document = json.loads(site_map.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            return fail(f"could not read site map: {error}")

        sites = document.get("sites")
        if not isinstance(sites, list) or len(sites) != 2:
            return fail("expected exactly two overloaded call sites", str(document))
        if {site.get("callee") for site in sites} != {"overloaded_status"}:
            return fail("overloaded sites did not share the expected callee name", str(document))
        callee_types = {site.get("callee_type") for site in sites}
        if len(callee_types) != 2 or not all(isinstance(value, str) and value for value in callee_types):
            return fail("overloaded sites did not have distinct canonical types", str(document))

        identifiers: set[int] = set()
        for site in sites:
            try:
                identifier = int(site["id"], 10)
                expected = expected_identifier(site)
            except (KeyError, TypeError, ValueError) as error:
                return fail(f"invalid site metadata: {error}", str(document))
            if identifier != expected:
                return fail(
                    f"site ID {identifier} did not include the canonical callee type; expected {expected}",
                    str(document),
                )
            identifiers.add(identifier)
        if len(identifiers) != 2:
            return fail("overloaded sites did not receive distinct IDs", str(document))

        object_file = directory / "instrumented.o"
        compilation = run(
            [
                str(compiler),
                "-std=c++20",
                "-Wall",
                "-Wextra",
                "-Wpedantic",
                "-Werror",
                "-I",
                str(include),
                "-c",
                str(transformed),
                "-o",
                str(object_file),
            ]
        )
        if compilation.returncode != 0:
            return fail("transformed overloaded source did not compile", compilation.stdout)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
