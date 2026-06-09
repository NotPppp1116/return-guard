#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import os
import pathlib
import signal
import stat
import subprocess
import sys
import tempfile
import time
import unittest


RUNNER: pathlib.Path


class ScalingTests(unittest.TestCase):
    def write_database(
        self,
        root: pathlib.Path,
        build: pathlib.Path,
        names: list[str],
    ) -> None:
        source = root / "src"
        source.mkdir(exist_ok=True)
        entries = []
        for name in names:
            path = source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("int value;\n", encoding="utf-8")
            entries.append(
                {
                    "directory": str(root),
                    "file": str(path),
                    "command": f"clang -c {path}",
                }
            )
        (build / "compile_commands.json").write_text(
            json.dumps(entries),
            encoding="utf-8",
        )

    def make_project(self, root: pathlib.Path, names: list[str]) -> pathlib.Path:
        build = root / "build"
        build.mkdir()
        self.write_database(root, build, names)
        return build

    def make_tool(
        self,
        root: pathlib.Path,
        *,
        failing_name: str | None = None,
        sleep_seconds: float = 0.0,
    ) -> pathlib.Path:
        tool = root / "fake-tool.py"
        tool.write_text(
            "#!/usr/bin/env python3\n"
            "import pathlib, sys, time\n"
            f"failing = {failing_name!r}\n"
            f"delay = {sleep_seconds!r}\n"
            "source = pathlib.Path(sys.argv[-1])\n"
            "if source.name != failing and delay:\n"
            "    time.sleep(delay)\n"
            "print(f'analyzed:{source.name}')\n"
            "raise SystemExit(1 if source.name == failing else 0)\n",
            encoding="utf-8",
        )
        tool.chmod(tool.stat().st_mode | stat.S_IXUSR)
        return tool

    def run_runner(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(RUNNER), *args],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

    def shard_mapping(
        self,
        *,
        root: pathlib.Path,
        build: pathlib.Path,
        shard_count: int,
    ) -> dict[str, int]:
        mapping: dict[str, int] = {}
        for shard_index in range(shard_count):
            result = self.run_runner(
                "-p",
                str(build),
                "--list-files",
                "--shard-root",
                str(root),
                "--shard-count",
                str(shard_count),
                "--shard-index",
                str(shard_index),
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            for line in result.stdout.splitlines():
                mapping[pathlib.Path(line).name] = shard_index
        return mapping

    def test_fail_fast_does_not_raise_on_cancelled_futures(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            build = self.make_project(
                root,
                ["bad.c", "slow1.c", "slow2.c", "slow3.c"],
            )
            tool = self.make_tool(root, failing_name="bad.c", sleep_seconds=0.1)
            result = self.run_runner(
                "-p",
                str(build),
                "--tool",
                str(tool),
                "--jobs",
                "2",
                "--fail-fast",
                "--progress-every",
                "0",
            )
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn("analyzed:bad.c", result.stdout)
            self.assertIn("stopped early", result.stderr)
            self.assertNotIn("Traceback", result.stderr)

    def test_shards_do_not_reshuffle_when_a_file_is_added(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            build = self.make_project(root, ["a.c", "c.c", "d.c", "e.c"])

            before = self.shard_mapping(
                root=root,
                build=build,
                shard_count=3,
            )
            self.write_database(
                root,
                build,
                ["a.c", "b.c", "c.c", "d.c", "e.c"],
            )
            after = self.shard_mapping(
                root=root,
                build=build,
                shard_count=3,
            )

            self.assertEqual(
                before,
                {name: after[name] for name in before},
            )

    def test_timeout_is_counted_as_a_failure(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            build = self.make_project(root, ["slow.c"])
            tool = self.make_tool(root, sleep_seconds=0.2)
            result = self.run_runner(
                "-p",
                str(build),
                "--tool",
                str(tool),
                "--timeout",
                "0.02",
                "--progress-every",
                "0",
            )
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn("timed out after", result.stdout)
            self.assertIn("1 timed out", result.stderr)

    @unittest.skipUnless(os.name == "posix", "requires POSIX process groups")
    def test_timeout_terminates_descendant_processes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            build = self.make_project(root, ["slow.c"])
            marker = root / "descendant-survived"
            tool = root / "spawn-tree.sh"
            tool.write_text(
                "#!/bin/sh\n"
                f"(sleep 0.25; echo alive > {marker!s}) &\n"
                "sleep 5\n",
                encoding="utf-8",
            )
            tool.chmod(tool.stat().st_mode | stat.S_IXUSR)

            result = self.run_runner(
                "-p",
                str(build),
                "--tool",
                str(tool),
                "--timeout",
                "0.05",
                "--progress-every",
                "0",
            )
            self.assertEqual(result.returncode, 1, result.stderr)
            time.sleep(0.35)
            self.assertFalse(marker.exists())

    @unittest.skipUnless(os.name == "posix", "requires POSIX signals")
    def test_interrupt_terminates_active_analysis(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            build = self.make_project(root, ["slow.c"])
            pid_file = root / "tool.pid"
            tool = root / "wait.sh"
            tool.write_text(
                "#!/bin/sh\n"
                f"echo $$ > {pid_file!s}\n"
                "sleep 10\n",
                encoding="utf-8",
            )
            tool.chmod(tool.stat().st_mode | stat.S_IXUSR)

            process = subprocess.Popen(
                [
                    sys.executable,
                    str(RUNNER),
                    "-p",
                    str(build),
                    "--tool",
                    str(tool),
                    "--progress-every",
                    "0",
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

            deadline = time.monotonic() + 5.0
            while not pid_file.exists() and time.monotonic() < deadline:
                if process.poll() is not None:
                    break
                time.sleep(0.02)

            self.assertTrue(pid_file.exists())
            started = time.monotonic()
            process.send_signal(signal.SIGINT)
            _stdout, stderr = process.communicate(timeout=3.0)
            elapsed = time.monotonic() - started

            self.assertEqual(process.returncode, 130, stderr)
            self.assertLess(elapsed, 2.0)
            self.assertIn("interrupted", stderr)
            self.assertNotIn("Traceback", stderr)

            tool_pid = int(pid_file.read_text(encoding="utf-8"))
            with self.assertRaises(ProcessLookupError):
                os.kill(tool_pid, 0)

    def test_logs_can_be_kept_without_streaming_to_stdout(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            build = self.make_project(root, ["one.c"])
            tool = self.make_tool(root)
            logs = root / "logs"
            result = self.run_runner(
                "-p",
                str(build),
                "--tool",
                str(tool),
                "--log-dir",
                str(logs),
                "--no-stream-output",
                "--progress-every",
                "0",
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, "")
            files = list(logs.glob("*.log"))
            self.assertEqual(len(files), 1)
            self.assertIn(
                "analyzed:one.c",
                files[0].read_text(encoding="utf-8"),
            )


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", required=True, type=pathlib.Path)
    parsed, remaining = parser.parse_known_args()
    RUNNER = parsed.runner.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
