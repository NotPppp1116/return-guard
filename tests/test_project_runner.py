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
            "source_arg = sys.argv[sys.argv.index('--') - 1] if '--' in sys.argv else sys.argv[-1]\n"
            "source = pathlib.Path(source_arg)\n"
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

    def test_lists_deduplicated_c_and_cpp_translation_units(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            build = self.make_database(
                root,
                ["alpha.c", "beta.c", "gamma.cpp", "ignored.py"],
                duplicate_first=True,
            )

            completed = self.run_runner(
                "-p",
                str(build),
                "--list-files",
            )
            lines = [pathlib.Path(line).name for line in completed.stdout.splitlines()]
            self.assertEqual(lines, ["alpha.c", "beta.c", "gamma.cpp"])

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

    def test_source_root_scan_lists_c_and_cpp_files_without_database(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / "src"
            source.mkdir()
            (source / "alpha.c").write_text("int alpha;\n", encoding="utf-8")
            (source / "beta.cpp").write_text("int beta;\n", encoding="utf-8")
            (source / "ignored.py").write_text("ignored = True\n", encoding="utf-8")
            build = root / "build"
            build.mkdir()
            (build / "generated.c").write_text("int generated;\n", encoding="utf-8")

            completed = self.run_runner(
                "--source-root",
                str(root),
                "--list-files",
            )

            lines = [pathlib.Path(line).name for line in completed.stdout.splitlines()]
            self.assertEqual(lines, ["alpha.c", "beta.cpp"])

    def test_source_root_scan_accepts_extra_excluded_directories(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / "src"
            vendor = source / "vendor"
            generated = source / "generated"
            vendor.mkdir(parents=True)
            generated.mkdir()
            (source / "alpha.c").write_text("int alpha;\n", encoding="utf-8")
            (vendor / "vendored.c").write_text("int vendored;\n", encoding="utf-8")
            (generated / "output.c").write_text("int output;\n", encoding="utf-8")

            completed = self.run_runner(
                "--source-root",
                str(root),
                "--scan-exclude-dir",
                "vendor,generated",
                "--list-files",
            )

            lines = [pathlib.Path(line).name for line in completed.stdout.splitlines()]
            self.assertEqual(lines, ["alpha.c"])

    def test_source_root_scan_rejects_excluded_directory_paths(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "src").mkdir()

            completed = self.run_runner(
                "--source-root",
                str(root),
                "--scan-exclude-dir",
                "vendor/generated",
                "--list-files",
                expected_code=2,
            )

            self.assertIn("directory basenames", completed.stderr)

    def test_source_root_dry_run_uses_compile_arguments(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / "src"
            include = root / "include"
            source.mkdir()
            include.mkdir()
            (source / "one.c").write_text("int one;\n", encoding="utf-8")
            tool = self.make_fake_tool(root)

            completed = self.run_runner(
                "--source-root",
                str(source),
                "--tool",
                str(tool),
                "--dry-run",
                f"--compile-arg=-I{include}",
            )

            self.assertIn(str(source / "one.c"), completed.stdout)
            self.assertIn("-- -std=c17", completed.stdout)
            self.assertIn(f"-I{include}", completed.stdout)
            self.assertNotIn(" -p ", completed.stdout)

            (source / "two.cpp").write_text("int two;\n", encoding="utf-8")
            cpp = self.run_runner(
                "--source-root",
                str(source),
                "--tool",
                str(tool),
                "--dry-run",
                "--extensions=.cpp",
            )
            self.assertIn(str(source / "two.cpp"), cpp.stdout)
            self.assertIn("-- -std=c++20", cpp.stdout)

            custom_standard = self.run_runner(
                "--source-root",
                str(source),
                "--tool",
                str(tool),
                "--dry-run",
                "--compile-arg=-std=c11",
            )
            self.assertIn("-- -std=c11", custom_standard.stdout)
            self.assertNotIn("-std=c17", custom_standard.stdout)

    def test_source_root_scan_runs_without_database(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / "src"
            source.mkdir()
            (source / "one.c").write_text("int one;\n", encoding="utf-8")
            (source / "two.c").write_text("int two;\n", encoding="utf-8")
            tool = self.make_fake_tool(root)

            completed = self.run_runner(
                "--source-root",
                str(source),
                "--tool",
                str(tool),
                "--jobs",
                "2",
                "--progress-every",
                "0",
            )

            self.assertIn("analyzed:one.c", completed.stdout)
            self.assertIn("analyzed:two.c", completed.stdout)
            self.assertIn("2/2 translation units", completed.stderr)

    def test_parallel_run_filters_and_propagates_failure(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            build = self.make_database(
                root,
                ["good.c", "bad.c", "included.cpp", "ignored.py"],
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
            self.assertIn("analyzed:included.cpp", completed.stdout)
            self.assertNotIn("ignored.py", completed.stdout)
            self.assertIn("3/3 translation units", completed.stderr)
            self.assertIn("1 failed", completed.stderr)

    def test_diagnostic_summary_is_grouped_and_sorted(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            build = self.make_database(root, ["alpha.c", "beta.c"])
            tool = root / "diagnostic-returnguard.py"
            tool.write_text(
                "#!/usr/bin/env python3\n"
                "import pathlib\n"
                "import sys\n"
                "source = pathlib.Path(sys.argv[-1])\n"
                "if source.name == 'alpha.c':\n"
                "    print(f'{source}:1:1: warning: returnguard: possible short write from \\'write\\': positive return may be smaller than requested byte count')\n"
                "    print(f'{source}:2:1: warning: returnguard: return value of \\'open\\' is consumed but not verified')\n"
                "else:\n"
                "    print(f'{source}:1:1: warning: returnguard: possible short write from \\'send\\': positive return may be smaller than requested byte count')\n",
                encoding="utf-8",
            )
            tool.chmod(tool.stat().st_mode | stat.S_IXUSR)

            completed = self.run_runner(
                "-p",
                str(build),
                "--tool",
                str(tool),
                "--progress-every",
                "0",
            )

            self.assertIn("diagnostic summary by type", completed.stderr)
            short_line = "      2  short write or send"
            consumed_line = "      1  unchecked consumed return"
            self.assertIn(short_line, completed.stderr)
            self.assertIn(consumed_line, completed.stderr)
            self.assertLess(
                completed.stderr.index(short_line),
                completed.stderr.index(consumed_line),
            )

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
