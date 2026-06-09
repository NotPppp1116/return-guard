#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import os
import pathlib
import stat
import subprocess
import sys
import tempfile
import unittest


RUNNER: pathlib.Path


class ProjectRunnerTests(unittest.TestCase):
    def make_database(
        self,
        root: pathlib.Path,
        names: list[str],
        *,
        duplicate_first: bool = False,
    ) -> pathlib.Path:
        build = root / "build"
        source = root / "src"
        build.mkdir()
        source.mkdir()

        entries = []
        for name in names:
            path = source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("int value;\n", encoding="utf-8")
            entries.append(
                {
                    "directory": str(root),
                    "file": str(path.relative_to(root)),
                    "command": f"clang -c {path}",
                }
            )
        if duplicate_first:
            entries.append(dict(entries[0]))

        database = build / "compile_commands.json"
        database.write_text(json.dumps(entries), encoding="utf-8")
        return build

    def make_fake_tool(
        self,
        root: pathlib.Path,
        *,
        failing_name: str | None = None,
    ) -> pathlib.Path:
        tool = root / "fake-returnguard.py"
        failing_literal = repr(failing_name)
        tool.write_text(
            "#!/usr/bin/env python3\n"
            "import pathlib\n"
            "import sys\n"
            f"failing = {failing_literal}\n"
            "source = pathlib.Path(sys.argv[-1])\n"
            "print(f'analyzed:{source.name}')\n"
            "raise SystemExit(1 if source.name == failing else 0)\n",
            encoding="utf-8",
        )
        tool.chmod(tool.stat().st_mode | stat.S_IXUSR)
        return tool

    def run_runner(
        self,
        *arguments: str,
        expected_code: int = 0,
    ) -> subprocess.CompletedProcess[str]:
        completed = subprocess.run(
            [sys.executable, str(RUNNER), *arguments],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(
            completed.returncode,
            expected_code,
            msg=f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}",
        )
        return completed

    def test_lists_deduplicated_c_translation_units(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            build = self.make_database(
                root,
                ["alpha.c", "beta.c", "ignored.cpp"],
                duplicate_first=True,
            )

            completed = self.run_runner(
                "-p",
                str(build),
                "--list-files",
            )
            lines = [pathlib.Path(line).name for line in completed.stdout.splitlines()]
            self.assertEqual(lines, ["alpha.c", "beta.c"])

    def test_relative_database_directory_is_resolved_from_database(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            build = self.make_database(root, ["relative.c"])
            database = build / "compile_commands.json"
            entries = json.loads(database.read_text(encoding="utf-8"))
            entries[0]["directory"] = ".."
            entries[0]["file"] = "src/relative.c"
            database.write_text(json.dumps(entries), encoding="utf-8")

            completed = self.run_runner(
                "-p",
                str(build),
                "--list-files",
            )

            self.assertEqual(
                completed.stdout.strip(),
                str((root / "src" / "relative.c").resolve()),
            )

    def test_parallel_run_filters_and_propagates_failure(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            build = self.make_database(
                root,
                ["good.c", "bad.c", "ignored.cpp"],
            )
            tool = self.make_fake_tool(root, failing_name="bad.c")

            completed = self.run_runner(
                "-p",
                str(build),
                "--tool",
                str(tool),
                "--jobs",
                "2",
                "--progress-every",
                "0",
                expected_code=1,
            )
            self.assertIn("analyzed:good.c", completed.stdout)
            self.assertIn("analyzed:bad.c", completed.stdout)
            self.assertNotIn("ignored.cpp", completed.stdout)
            self.assertIn("2/2 translation units", completed.stderr)
            self.assertIn("1 failed", completed.stderr)

    def test_include_exclude_and_shards_are_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            build = self.make_database(
                root,
                ["a.c", "b.c", "generated/c.c", "d.c"],
            )

            shard_zero = self.run_runner(
                "-p",
                str(build),
                "--list-files",
                "--exclude-regex",
                "generated",
                "--shard-count",
                "2",
                "--shard-index",
                "0",
            )
            shard_one = self.run_runner(
                "-p",
                str(build),
                "--list-files",
                "--exclude-regex",
                "generated",
                "--shard-count",
                "2",
                "--shard-index",
                "1",
            )

            zero = set(shard_zero.stdout.splitlines())
            one = set(shard_one.stdout.splitlines())
            self.assertFalse(zero & one)
            self.assertEqual(len(zero | one), 3)

    def test_invalid_database_is_reported_as_usage_error(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            build = root / "build"
            build.mkdir()
            (build / "compile_commands.json").write_text("not-json", encoding="utf-8")

            completed = self.run_runner(
                "-p",
                str(build),
                "--list-files",
                expected_code=2,
            )
            self.assertIn("invalid JSON", completed.stderr)


if __name__ == "__main__":
    argument_parser = argparse.ArgumentParser()
    argument_parser.add_argument("--runner", required=True, type=pathlib.Path)
    parsed, unittest_arguments = argument_parser.parse_known_args()
    RUNNER = parsed.runner.resolve()
    unittest.main(argv=[sys.argv[0], *unittest_arguments])
