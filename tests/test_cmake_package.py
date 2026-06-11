#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import pathlib
import shutil
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
    print(f"CMake package test: {message}", file=sys.stderr)
    if output:
        print(output, file=sys.stderr)
    return 1


def read_site_map(path: pathlib.Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def single_per_object_map(directory: pathlib.Path) -> pathlib.Path:
    maps = sorted(directory.glob("*.json"))
    if len(maps) != 1:
        raise RuntimeError(f"expected one per-object site map, found {maps}")
    return maps[0]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--compiler", required=True)
    arguments = parser.parse_args()

    cmake = pathlib.Path(arguments.cmake).resolve()
    returnguard_build = pathlib.Path(arguments.build_dir).resolve()
    compiler = pathlib.Path(arguments.compiler).resolve()

    with tempfile.TemporaryDirectory(prefix="returnguard-package-") as directory_name:
        directory = pathlib.Path(directory_name)
        prefix = directory / "prefix"
        source_dir = directory / "sample"
        build_dir = directory / "sample-build"
        source_dir.mkdir()

        install = run(
            [
                str(cmake),
                "--install",
                str(returnguard_build),
                "--prefix",
                str(prefix),
            ]
        )
        if install.returncode != 0:
            return fail("installing ReturnGuard failed", install.stdout)

        merger = prefix / "bin" / "returnguard-site-map"
        if not merger.is_file():
            return fail("installed site-map merger was not found")

        (source_dir / "CMakeLists.txt").write_text(
            """cmake_minimum_required(VERSION 3.20)
project(returnguard_package_consumer LANGUAGES C)
find_package(ReturnGuard CONFIG REQUIRED)
add_executable(hardened main.c)
returnguard_harden_target(hardened)
""",
            encoding="utf-8",
        )
        (source_dir / "main.c").write_text(
            """#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include <returnguard/Contracts.h>
#include <returnguard/Runtime.h>

static unsigned char secret[] = {1U, 2U, 3U, 4U};
static unsigned char removed[] = {5U, 6U, 7U, 8U};

static int unavailable(void) RETURNGUARD_FAILS_NEGATIVE;
static int unavailable(void) { return -1; }
static int consume(int value) { return value; }

static void write_site_id(uint64_t site_id) {
    static const char hex[] = "0123456789abcdef";
    char text[17];
    for (unsigned index = 0U; index < 16U; ++index) {
        const unsigned shift = (15U - index) * 4U;
        text[index] = hex[(site_id >> shift) & 0x0fU];
    }
    text[16] = '\\n';
    (void)write(2, "site-id=", 8U);
    (void)write(2, text, sizeof(text));
}

void __rg_fatal_hook(uint64_t site_id, int saved_errno) {
    (void)saved_errno;
    const char wiped[] = "secret-wiped\\n";
    const char not_wiped[] = "secret-not-wiped\\n";
    const int registered_was_wiped =
        secret[0] == 0U && secret[1] == 0U &&
        secret[2] == 0U && secret[3] == 0U;
    const int removed_was_preserved =
        removed[0] == 5U && removed[1] == 6U &&
        removed[2] == 7U && removed[3] == 8U;
    write_site_id(site_id);
    if (registered_was_wiped != 0 && removed_was_preserved != 0) {
        (void)write(2, wiped, sizeof(wiped) - 1U);
    } else {
        (void)write(2, not_wiped, sizeof(not_wiped) - 1U);
    }
}

int main(void) {
    if (returnguard_register_secret(secret, sizeof(secret)) !=
        RETURNGUARD_SECRET_OK) {
        return 10;
    }
    if (returnguard_register_secret(secret, sizeof(secret)) !=
        RETURNGUARD_SECRET_ALREADY_REGISTERED) {
        return 11;
    }
    if (returnguard_register_secret(removed, sizeof(removed)) !=
        RETURNGUARD_SECRET_OK) {
        return 12;
    }
    if (returnguard_unregister_secret(removed) != RETURNGUARD_SECRET_OK) {
        return 13;
    }
    if (returnguard_unregister_secret(removed) !=
        RETURNGUARD_SECRET_NOT_FOUND) {
        return 14;
    }
    return consume(unavailable());
}
""",
            encoding="utf-8",
        )

        configure = run(
            [
                str(cmake),
                "-S",
                str(source_dir),
                "-B",
                str(build_dir),
                f"-DCMAKE_PREFIX_PATH={prefix}",
                f"-DCMAKE_C_COMPILER={compiler}",
            ]
        )
        if configure.returncode != 0:
            return fail("consumer configuration failed", configure.stdout)

        build = run([str(cmake), "--build", str(build_dir)])
        if build.returncode != 0:
            return fail("consumer hardened build failed", build.stdout)

        program = build_dir / "hardened"
        if not program.is_file():
            return fail("consumer build did not produce the hardened executable")

        site_map_directory = build_dir / "returnguard-sites" / "hardened"
        try:
            per_object_map = single_per_object_map(site_map_directory)
            first_document = read_site_map(per_object_map)
        except (OSError, RuntimeError, json.JSONDecodeError) as error:
            return fail(f"could not read generated site map: {error}")

        if first_document.get("schema_version") != 1:
            return fail("generated site map has the wrong schema version")
        sites = first_document.get("sites")
        if not isinstance(sites, list) or len(sites) != 1:
            return fail("generated site map did not contain exactly one site", str(first_document))
        site = sites[0]
        expected_fields = {
            "file": "main.c",
            "function": "main",
            "callee": "unavailable",
            "predicate": "negative",
        }
        for field, expected in expected_fields.items():
            if site.get(field) != expected:
                return fail(
                    f"generated site field {field!r} was {site.get(field)!r}, expected {expected!r}",
                    str(first_document),
                )
        try:
            manifest_identifier = int(site["id"], 10)
        except (KeyError, TypeError, ValueError):
            return fail("generated site ID was not a decimal string", str(first_document))
        if manifest_identifier <= 0 or manifest_identifier > 0xFFFFFFFFFFFFFFFF:
            return fail("generated site ID was outside the 64-bit range")

        execution = run([str(program)])
        if execution.returncode != 127:
            return fail(
                f"installed-package program exited with {execution.returncode}, expected 127",
                execution.stdout,
            )
        if "secret-wiped" not in execution.stdout or "secret-not-wiped" in execution.stdout:
            return fail("registered secret was not wiped before the hook", execution.stdout)
        runtime_identifier_text = next(
            (
                line.removeprefix("site-id=")
                for line in execution.stdout.splitlines()
                if line.startswith("site-id=")
            ),
            None,
        )
        if runtime_identifier_text is None:
            return fail("fatal hook did not report its site ID", execution.stdout)
        try:
            runtime_identifier = int(runtime_identifier_text, 16)
        except ValueError:
            return fail("fatal hook reported an invalid hexadecimal site ID", execution.stdout)
        if runtime_identifier != manifest_identifier:
            return fail(
                f"runtime site ID {runtime_identifier} did not match manifest ID {manifest_identifier}",
                execution.stdout,
            )

        combined_map = build_dir / "returnguard-sites.json"
        merge = run(
            [
                str(merger),
                "--input-dir",
                str(site_map_directory),
                "--output",
                str(combined_map),
            ]
        )
        if merge.returncode != 0:
            return fail("site-map merge failed", merge.stdout)
        combined_document = read_site_map(combined_map)
        if combined_document.get("sites") != sites:
            return fail("merged site map changed the site metadata", str(combined_document))

        clean = run([str(cmake), "--build", str(build_dir), "--target", "clean"])
        if clean.returncode != 0:
            return fail("consumer clean failed", clean.stdout)
        if site_map_directory.exists() and list(site_map_directory.glob("*.json")):
            return fail("CMake clean left stale per-object site maps")

        rebuild = run([str(cmake), "--build", str(build_dir)])
        if rebuild.returncode != 0:
            return fail("consumer rebuild failed", rebuild.stdout)
        try:
            rebuilt_document = read_site_map(single_per_object_map(site_map_directory))
        except (OSError, RuntimeError, json.JSONDecodeError) as error:
            return fail(f"could not read rebuilt site map: {error}")
        if rebuilt_document.get("sites") != sites:
            return fail("site metadata was not stable across a clean rebuild")

        collision_directory = directory / "collision-maps"
        collision_directory.mkdir()
        original_collision_map = collision_directory / "original.json"
        conflicting_map = collision_directory / "conflicting.json"
        shutil.copy2(single_per_object_map(site_map_directory), original_collision_map)
        conflicting_document = json.loads(json.dumps(rebuilt_document))
        conflicting_document["sites"][0]["callee"] = "different_function"
        conflicting_map.write_text(
            json.dumps(conflicting_document, indent=2) + "\n", encoding="utf-8"
        )
        collision_output = directory / "collision-output.json"
        collision = run(
            [
                str(merger),
                "--input-dir",
                str(collision_directory),
                "--output",
                str(collision_output),
            ]
        )
        if collision.returncode != 2 or "site ID collision" not in collision.stdout:
            return fail("site-map merger did not reject a forged collision", collision.stdout)
        if collision_output.exists():
            return fail("collision validation left a misleading merged output")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
