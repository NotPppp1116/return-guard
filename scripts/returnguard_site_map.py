#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import os
import pathlib
import sys
import tempfile
from collections.abc import Iterable
from typing import Any

SCHEMA_VERSION = 1
SITE_FIELDS = (
    "id",
    "file",
    "line",
    "column",
    "function",
    "callee",
    "predicate",
)


class SiteMapError(RuntimeError):
    pass


def discover_maps(
    input_directories: Iterable[pathlib.Path], output: pathlib.Path
) -> list[pathlib.Path]:
    output = output.resolve(strict=False)
    maps: set[pathlib.Path] = set()
    for directory in input_directories:
        directory = directory.expanduser().resolve(strict=False)
        if not directory.is_dir():
            raise SiteMapError(f"input directory does not exist: {directory}")
        for candidate in directory.rglob("*.json"):
            candidate = candidate.resolve(strict=False)
            if candidate != output:
                maps.add(candidate)
    if not maps:
        raise SiteMapError("no site-map JSON files were found")
    return sorted(maps)


def require_string(site: dict[str, Any], field: str, path: pathlib.Path) -> str:
    value = site.get(field)
    if not isinstance(value, str):
        raise SiteMapError(f"{path}: site field {field!r} must be a string")
    return value


def require_nonnegative_integer(
    site: dict[str, Any], field: str, path: pathlib.Path
) -> int:
    value = site.get(field)
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise SiteMapError(
            f"{path}: site field {field!r} must be a non-negative integer"
        )
    return value


def validate_site(raw: Any, path: pathlib.Path) -> dict[str, Any]:
    if not isinstance(raw, dict):
        raise SiteMapError(f"{path}: every site entry must be an object")

    missing = [field for field in SITE_FIELDS if field not in raw]
    if missing:
        raise SiteMapError(f"{path}: site entry is missing {', '.join(missing)}")

    identifier_text = require_string(raw, "id", path)
    try:
        identifier = int(identifier_text, 10)
    except ValueError as error:
        raise SiteMapError(
            f"{path}: invalid decimal site ID {identifier_text!r}"
        ) from error
    if identifier <= 0 or identifier > 0xFFFFFFFFFFFFFFFF:
        raise SiteMapError(f"{path}: site ID is outside the unsigned 64-bit range")

    predicate = require_string(raw, "predicate", path)
    if predicate not in {"null", "negative"}:
        raise SiteMapError(f"{path}: unsupported predicate {predicate!r}")

    return {
        "id": str(identifier),
        "file": require_string(raw, "file", path),
        "line": require_nonnegative_integer(raw, "line", path),
        "column": require_nonnegative_integer(raw, "column", path),
        "function": require_string(raw, "function", path),
        "callee": require_string(raw, "callee", path),
        "predicate": predicate,
    }


def read_map(path: pathlib.Path) -> list[dict[str, Any]]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise SiteMapError(f"cannot read {path}: {error}") from error

    if not isinstance(document, dict):
        raise SiteMapError(f"{path}: top-level JSON value must be an object")
    if document.get("schema_version") != SCHEMA_VERSION:
        raise SiteMapError(
            f"{path}: unsupported schema version {document.get('schema_version')!r}"
        )
    sites = document.get("sites")
    if not isinstance(sites, list):
        raise SiteMapError(f"{path}: 'sites' must be an array")
    return [validate_site(site, path) for site in sites]


def logical_key(site: dict[str, Any]) -> tuple[Any, ...]:
    return tuple(site[field] for field in SITE_FIELDS if field != "id")


def merge_maps(paths: Iterable[pathlib.Path]) -> list[dict[str, Any]]:
    by_identifier: dict[str, tuple[dict[str, Any], pathlib.Path]] = {}
    by_logical_site: dict[tuple[Any, ...], tuple[str, pathlib.Path]] = {}

    for path in paths:
        for site in read_map(path):
            identifier = site["id"]
            existing = by_identifier.get(identifier)
            if existing is not None and existing[0] != site:
                raise SiteMapError(
                    f"site ID collision {identifier}: {existing[1]} and {path} describe different sites"
                )
            by_identifier.setdefault(identifier, (site, path))

            key = logical_key(site)
            existing_logical = by_logical_site.get(key)
            if existing_logical is not None and existing_logical[0] != identifier:
                raise SiteMapError(
                    f"logical site has inconsistent IDs {existing_logical[0]} and {identifier}: "
                    f"{existing_logical[1]} and {path}"
                )
            by_logical_site.setdefault(key, (identifier, path))

    return [
        entry[0]
        for _, entry in sorted(
            by_identifier.items(), key=lambda item: int(item[0], 10)
        )
    ]


def write_atomic(output: pathlib.Path, sites: list[dict[str, Any]]) -> None:
    output = output.expanduser().resolve(strict=False)
    output.parent.mkdir(parents=True, exist_ok=True)
    document = {"schema_version": SCHEMA_VERSION, "sites": sites}

    temporary_path: pathlib.Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            dir=output.parent,
            prefix=f".{output.name}.",
            suffix=".tmp",
            delete=False,
        ) as temporary:
            temporary_path = pathlib.Path(temporary.name)
            json.dump(document, temporary, indent=2, sort_keys=False)
            temporary.write("\n")
            temporary.flush()
            os.fsync(temporary.fileno())
        os.replace(temporary_path, output)
    finally:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Merge ReturnGuard per-object site maps and reject collisions"
    )
    parser.add_argument(
        "--input-dir",
        action="append",
        required=True,
        type=pathlib.Path,
        help="directory containing per-object JSON site maps; may be repeated",
    )
    parser.add_argument("--output", required=True, type=pathlib.Path)
    arguments = parser.parse_args()

    try:
        output = arguments.output.expanduser().resolve(strict=False)
        output.unlink(missing_ok=True)
        paths = discover_maps(arguments.input_dir, output)
        sites = merge_maps(paths)
        write_atomic(output, sites)
    except SiteMapError as error:
        print(f"returnguard-site-map: {error}", file=sys.stderr)
        return 2
    except OSError as error:
        print(f"returnguard-site-map: {error}", file=sys.stderr)
        return 2

    print(
        f"returnguard-site-map: wrote {len(sites)} sites from {len(paths)} files to {output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
