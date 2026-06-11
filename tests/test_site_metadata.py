#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import pathlib
import re
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
    print(f"site metadata test: {message}", file=sys.stderr)
    if output:
        print(output, file=sys.stderr)
    return 1


def transform(
    *,
    tool: pathlib.Path,
    source: pathlib.Path,
    include: pathlib.Path,
    transformed: pathlib.Path,
    site_map: pathlib.Path,
) -> subprocess.CompletedProcess[str]:
    return run(
        [
            str(tool),
            "--no-color",
            f"--instrument-output={transformed}",
            f"--site-map-output={site_map}",
            f"--site-root={source.parent}",
            str(source),
            "--",
            "-std=c17",
            "-I",
            str(include),
        ]
    )


def read_document(path: pathlib.Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def validate_document(document: dict[str, Any]) -> tuple[bool, str]:
    if set(document) != {"schema_version", "sites"}:
        return False, f"unexpected top-level fields: {sorted(document)}"
    if document.get("schema_version") != 1:
        return False, f"unexpected schema version: {document.get('schema_version')!r}"

    sites = document.get("sites")
    if not isinstance(sites, list) or len(sites) != 3:
        return False, f"expected three sites, got {sites!r}"

    expected = {
        "malloc": "null",
        "open": "negative",
        "close": "negative",
    }
    observed: dict[str, str] = {}
    identifiers: set[int] = set()
    for site in sites:
        if not isinstance(site, dict):
            return False, f"site was not an object: {site!r}"
        if site.get("file") != "instrumentation.c":
            return False, f"site path was not source-root relative: {site!r}"
        if site.get("function") != "instrumentation_sample":
            return False, f"wrong enclosing function: {site!r}"
        if not isinstance(site.get("line"), int) or site["line"] <= 0:
            return False, f"invalid line: {site!r}"
        if not isinstance(site.get("column"), int) or site["column"] <= 0:
            return False, f"invalid column: {site!r}"

        callee = site.get("callee")
        predicate = site.get("predicate")
        if not isinstance(callee, str) or not isinstance(predicate, str):
            return False, f"invalid callee or predicate: {site!r}"
        observed[callee] = predicate

        identifier_text = site.get("id")
        if not isinstance(identifier_text, str):
            return False, f"site ID was not a string: {site!r}"
        try:
            identifier = int(identifier_text, 10)
        except ValueError:
            return False, f"site ID was not decimal: {site!r}"
        if identifier <= 0 or identifier > 0xFFFFFFFFFFFFFFFF:
            return False, f"site ID was outside the unsigned 64-bit range: {site!r}"
        if identifier in identifiers:
            return False, f"duplicate site ID: {identifier}"
        identifiers.add(identifier)

    if observed != expected:
        return False, f"wrong callee/predicate map: {observed!r}"
    return True, ""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--invalid-source", required=True)
    parser.add_argument("--include", required=True)
    arguments = parser.parse_args()

    tool = pathlib.Path(arguments.tool).resolve()
    source = pathlib.Path(arguments.source).resolve()
    invalid_source = pathlib.Path(arguments.invalid_source).resolve()
    include = pathlib.Path(arguments.include).resolve()

    with tempfile.TemporaryDirectory(prefix="returnguard-site-metadata-") as name:
        directory = pathlib.Path(name)
        transformed = directory / "instrumented.c"
        site_map = directory / "sites.json"
        first = transform(
            tool=tool,
            source=source,
            include=include,
            transformed=transformed,
            site_map=site_map,
        )
        if first.returncode != 0:
            return fail("first transformation failed", first.stdout)
        if not transformed.is_file() or not site_map.is_file():
            return fail("transformation did not create both outputs")

        try:
            first_document = read_document(site_map)
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            return fail(f"could not parse first site map: {error}")
        valid, message = validate_document(first_document)
        if not valid:
            return fail(message, site_map.read_text(encoding="utf-8"))

        transformed_text = transformed.read_text(encoding="utf-8")
        wrapper_identifiers = {
            int(value, 10)
            for value in re.findall(r", ([0-9]+)ULL\)", transformed_text)
        }
        manifest_identifiers = {
            int(site["id"], 10) for site in first_document["sites"]
        }
        if wrapper_identifiers != manifest_identifiers:
            return fail(
                "wrapper IDs did not match manifest IDs",
                transformed_text + "\n" + site_map.read_text(encoding="utf-8"),
            )

        second_transformed = directory / "instrumented-second.c"
        second_site_map = directory / "sites-second.json"
        second = transform(
            tool=tool,
            source=source,
            include=include,
            transformed=second_transformed,
            site_map=second_site_map,
        )
        if second.returncode != 0:
            return fail("second transformation failed", second.stdout)
        if site_map.read_bytes() != second_site_map.read_bytes():
            return fail("site map was not byte-for-byte deterministic")

        missing_instrument = run(
            [
                str(tool),
                "--no-color",
                f"--site-map-output={directory / 'invalid.json'}",
                str(source),
                "--",
                "-std=c17",
                "-I",
                str(include),
            ]
        )
        if missing_instrument.returncode != 2 or "requires --instrument-output" not in missing_instrument.stdout:
            return fail("--site-map-output validation did not fail closed", missing_instrument.stdout)

        missing_map = run(
            [
                str(tool),
                "--no-color",
                f"--instrument-output={directory / 'invalid.c'}",
                f"--site-root={source.parent}",
                str(source),
                "--",
                "-std=c17",
                "-I",
                str(include),
            ]
        )
        if missing_map.returncode != 2 or "requires --site-map-output" not in missing_map.stdout:
            return fail("--site-root validation did not fail closed", missing_map.stdout)

        stale_source = directory / "stale.c"
        stale_map = directory / "stale.json"
        stale_source.write_text("stale source\n", encoding="utf-8")
        stale_map.write_text("stale map\n", encoding="utf-8")
        invalid = transform(
            tool=tool,
            source=invalid_source,
            include=include,
            transformed=stale_source,
            site_map=stale_map,
        )
        if invalid.returncode == 0:
            return fail("invalid contracts unexpectedly transformed successfully")
        if stale_source.exists() or stale_map.exists():
            return fail("failed transformation left stale output files")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
