"""Offline checks for the external oracle runner and historical job auditor."""

from __future__ import annotations

import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path

from audit_job import audit
from run_benchmark import build_command, main as run_main


class RunnerTests(unittest.TestCase):
    def test_clanker_gates_are_removed(self) -> None:
        for gate in ("canary", "full"):
            with (self.subTest(gate=gate), contextlib.redirect_stderr(io.StringIO()),
                  self.assertRaises(SystemExit) as raised):
                run_main([gate, "--dry-run"])
            self.assertEqual(raised.exception.code, 2)

    def test_clanker_options_are_removed(self) -> None:
        options = {
            "--clanker-binary": "clanker",
            "--model": "example",
            "--effort": "high",
            "--max-retries": "1",
            "--retry-max-delay-ms": "1000",
            "--confirm-full-sweep": None,
        }
        for option, value in options.items():
            argv = ["oracle", "--dry-run", option]
            if value is not None:
                argv.append(value)
            with (self.subTest(option=option), contextlib.redirect_stderr(io.StringIO()),
                  self.assertRaises(SystemExit) as raised):
                run_main(argv)
            self.assertEqual(raised.exception.code, 2)

    def test_invalid_concurrency_is_rejected_before_running(self) -> None:
        for value in ("0", "-1"):
            with (self.subTest(value=value), contextlib.redirect_stderr(io.StringIO()),
                  self.assertRaises(SystemExit) as raised):
                run_main(["oracle", "--dry-run", "--concurrency", value])
            self.assertEqual(raised.exception.code, 2)

    def test_oracle_has_no_clanker_agent(self) -> None:
        command = build_command(Path("tasks"), "modal", 8)
        self.assertEqual(command[command.index("--agent") + 1], "oracle")
        self.assertNotIn("--agent-import-path", command)
        self.assertEqual(command[command.index("-r") + 1], "0")
        self.assertEqual(command[command.index("-n") + 1], "8")

    def test_oracle_dry_run_needs_no_binary_or_harness(self) -> None:
        with contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(run_main(["oracle", "--dry-run"]), 0)
        self.assertIn("--agent oracle", output.getvalue())


class AuditTests(unittest.TestCase):
    def _job(
        self,
        root: Path,
        *,
        pier_retries: int = 0,
        stop: str = "stop",
        completed_trials: int = 1,
    ) -> Path:
        job = root / "job"
        trial = job / "task__abc"
        (trial / "agent").mkdir(parents=True)
        (trial / "artifacts").mkdir()
        (job / "result.json").write_text(
            json.dumps(
                {
                    "n_total_trials": 1,
                    "stats": {
                        "n_completed_trials": completed_trials,
                        "n_errored_trials": 0,
                        "n_retries": pier_retries,
                    },
                }
            )
        )
        (trial / "result.json").write_text(
            json.dumps(
                {
                    "trial_name": "task__abc",
                    "agent_info": {
                        "name": "clanker",
                        "model_info": {"name": "claude-sonnet-4-6"},
                    },
                    "exception_info": None,
                }
            )
        )
        (trial / "artifacts" / "model.patch").write_text("diff --git a/a b/a\n")
        events = [
            {"type": "retry_start"},
            {
                "type": "record",
                "kind": "message",
                "record": {
                    "payload": {
                        "message": {
                            "role": "assistant",
                            "stopReason": stop,
                            "usage": {
                                "input": 100,
                                "output": 20,
                                "cacheRead": 7,
                                "cacheWrite": 3,
                            },
                        }
                    }
                },
            },
        ]
        (trial / "agent" / "clanker.ndjson").write_text(
            "\n".join(json.dumps(event) for event in events)
        )
        return job

    def test_clean_job_reports_request_retries_and_cost(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            report = audit(self._job(Path(tmp)))
        self.assertEqual(report["request_retries"], 1)
        self.assertEqual(report["pier_retries"], 0)
        self.assertEqual(report["issues"], [])
        self.assertAlmostEqual(report["estimated_cost_usd"], 0.00061335)

    def test_pier_retries_and_truncation_fail_audit(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            report = audit(self._job(Path(tmp), pier_retries=2, stop="length"))
        self.assertTrue(any("deleted failed-attempt evidence" in i for i in report["issues"]))
        self.assertTrue(any("truncated" in i for i in report["issues"]))

    def test_incomplete_job_fails_audit(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            report = audit(self._job(Path(tmp), completed_trials=0))
        self.assertTrue(any("job is incomplete" in i for i in report["issues"]))

    def test_empty_patch_warns_without_invalidating_score(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            job = self._job(Path(tmp))
            (job / "task__abc" / "artifacts" / "model.patch").write_text("")
            report = audit(job)
        self.assertEqual(report["issues"], [])
        self.assertEqual(report["empty_patches"], 1)
        self.assertTrue(any("scored zero" in warning for warning in report["warnings"]))


if __name__ == "__main__":
    unittest.main()
