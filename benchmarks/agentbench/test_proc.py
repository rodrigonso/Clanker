"""Offline checks for macOS accounting and portable CLI discovery."""

from __future__ import annotations

import contextlib
import io
import os
import subprocess
import unittest
from pathlib import Path
from unittest.mock import patch

from . import proc
from .__main__ import build_parser, main
from .tools import REPO, Tool, registry


class ProcessTests(unittest.TestCase):
    def test_ps_snapshot_parses_ancestry_groups_and_rss(self) -> None:
        result = subprocess.CompletedProcess(
            [], 0, stdout=" 10 1 10 1024\n11 10 10 512\nbad row\n12 1 12 0\n"
        )
        with patch.object(proc.subprocess, "run", return_value=result) as run:
            table = proc.proc_table()
        self.assertEqual(table, {10: (1, 10, 1024), 11: (10, 10, 512), 12: (1, 12, 0)})
        self.assertEqual(run.call_args.args[0],
                         ["/bin/ps", "-axo", "pid=,ppid=,pgid=,rss="])
        self.assertTrue(run.call_args.kwargs["check"])

    def test_tree_includes_reparented_helpers_and_separate_daemon_once(self) -> None:
        table = {
            10: (1, 10, 1024),
            11: (10, 10, 1024),
            12: (1, 10, 1024),
            20: (1, 20, 1024),
            21: (20, 21, 1024),
            30: (1, 30, 8192),
            40: (10, 10, 0),
        }
        with patch.object(proc, "proc_table", return_value=table) as snapshot:
            total, count = proc.tree_rss_mb([10, 20, 999], [10, 20])
        self.assertEqual((total, count), (5.0, 5))
        snapshot.assert_called_once_with()

    def test_process_table_failure_does_not_report_zero_memory(self) -> None:
        error = subprocess.CalledProcessError(1, "ps")
        with patch.object(proc.subprocess, "run", side_effect=error):
            with self.assertRaises(subprocess.CalledProcessError):
                proc.tree_rss_mb([10], [10])

    def test_native_executable_lookup_decodes_and_resolves_path(self) -> None:
        path = str(Path(__file__).resolve())

        def pidpath(pid, buffer, size):
            self.assertEqual(pid, 42)
            self.assertEqual(size, 4096)
            buffer.value = os.fsencode(path)
            return len(buffer.value)

        with patch.object(proc, "_proc_pidpath", side_effect=pidpath):
            self.assertEqual(proc.read_exe(42), os.path.realpath(path))

    def test_exited_or_unreadable_process_has_no_executable(self) -> None:
        with patch.object(proc, "_proc_pidpath", return_value=0):
            self.assertEqual(proc.read_exe(42), "")

    def test_live_scan_uses_snapshot_rss_and_excludes_itself(self) -> None:
        binary = str(Path(__file__).resolve())
        tool = Tool(name="clanker", binary=binary)
        table = {10: (1, 10, 1024), 11: (1, 11, 2048), 99: (1, 99, 4096)}
        with (
            patch.object(proc, "proc_table", return_value=table),
            patch.object(proc, "read_exe", return_value=tool.real_binary),
            patch.object(proc, "read_cmdline", return_value=binary),
        ):
            self.assertEqual(proc.scan([tool], exclude_pid=99), {"clanker": (3.0, 2)})


class DiscoveryTests(unittest.TestCase):
    def test_registry_falls_back_to_native_repository_build(self) -> None:
        with patch.dict(os.environ, {}, clear=True), patch("shutil.which", return_value=None):
            tools = registry()
        self.assertEqual(tools["clanker"].binary, str(REPO / "build/clanker"))
        self.assertEqual(tools["clanker"].env, {"PATH": ""})

    def test_registry_prefers_explicit_binary_then_path(self) -> None:
        with (
            patch.dict(os.environ, {"CLANKER_BIN": "~/native-clanker",
                                    "HOME": str(REPO), "USERPROFILE": str(REPO)}, clear=True),
            patch("shutil.which", return_value="from-path"),
        ):
            tools = registry()
            expected = os.path.expanduser("~/native-clanker")
        self.assertEqual(tools["clanker"].binary, expected)
        self.assertEqual(tools["pi"].binary, "from-path")

    def test_cli_help_uses_rss_without_importing_pty_modules(self) -> None:
        parser = build_parser()
        self.assertIn("macOS", parser.description)
        self.assertIn("RSS", parser.description)
        self.assertEqual(parser.parse_args(["measure"]).cwd, "out/agentbench-work")

    def test_interactive_commands_reject_non_macos_before_pty_imports(self) -> None:
        for platform in ("win32", "linux"):
            with (
                self.subTest(platform=platform),
                patch("sys.platform", platform),
                contextlib.redirect_stderr(io.StringIO()) as output,
            ):
                self.assertEqual(main(["doctor"]), 1)
                self.assertIn("clanker_tests --benchmark", output.getvalue())


if __name__ == "__main__":
    unittest.main()
