#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import pathlib
import stat
import subprocess
import sys
import tempfile
import unittest


RUNNER: pathlib.Path


class ScalingTests(unittest.TestCase):
    def make_project(self, root: pathlib.Path, names: list[str]) -> pathlib.Path:
        build = root / "build"
        source = root / "src"
        build.mkdir()
        source.mkdir()
        entries = []
        for name in names:
            path = source / name
            path.write_text("int value;\n", encoding="utf-8")
            entries.append(
                {
                    "directory": str(root),
                    "file": str(path),
                    "command": f"clang -c {path}",
                }
            )
        (build / "compile_commands.json").write_text(
            json.dumps(entries), encoding="utf-8"
        )
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

    def test_fail_fast_does_not_raise_on_cancelled_futures(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            build = self.make_project(
                root, ["bad.c", "slow1.c", "slow2.c", "slow3.c"]
            )
            tool = self.make_tool(root, failing_name="bad.c", sleep_seconds=0.1)
            result = self.run_runner(
                "-p", str(build),
                "--tool", str(tool),
                "--jobs", "2",
                "--fail-fast",
                "--progress-every", "0",
            )
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn("analyzed:bad.c", result.stdout)
            self.assertIn("stopped early", result.stderr)
            self.assertNotIn("Traceback", result.stderr)

    def test_timeout_is_counted_as_a_failure(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            build = self.make_project(root, ["slow.c"])
            tool = self.make_tool(root, sleep_seconds=0.2)
            result = self.run_runner(
                "-p", str(build),
                "--tool", str(tool),
                "--timeout", "0.02",
                "--progress-every", "0",
            )
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn("timed out after", result.stdout)
            self.assertIn("1 timed out", result.stderr)

    def test_logs_can_be_kept_without_streaming_to_stdout(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            build = self.make_project(root, ["one.c"])
            tool = self.make_tool(root)
            logs = root / "logs"
            result = self.run_runner(
                "-p", str(build),
                "--tool", str(tool),
                "--log-dir", str(logs),
                "--no-stream-output",
                "--progress-every", "0",
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, "")
            files = list(logs.glob("*.log"))
            self.assertEqual(len(files), 1)
            self.assertIn("analyzed:one.c", files[0].read_text(encoding="utf-8"))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", required=True, type=pathlib.Path)
    parsed, remaining = parser.parse_known_args()
    RUNNER = parsed.runner.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
