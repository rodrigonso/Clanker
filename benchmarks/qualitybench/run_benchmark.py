"""Run qualitybench with evidence-preserving defaults.

Pier retries remove a failed trial directory before starting the next attempt.
This wrapper fixes Pier retries at zero and puts the retry budget inside Clanker,
where a transient provider failure does not discard agent progress or logs.
"""

from __future__ import annotations

import argparse
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

DEFAULT_MAX_RETRIES = 8
DEFAULT_RETRY_MAX_DELAY_MS = 60000
DEFAULT_CONCURRENCY = 4
MAX_RETRIES = 1000
MAX_U32 = (1 << 32) - 1
FULL_SWEEP_COST = "$700-$900 in Anthropic API credits, plus Modal compute"
MODELS = {
    "canary": "anthropic/claude-sonnet-4-6",
    "full": "anthropic/claude-opus-4-8",
}
PROVIDER_KEY = "OPENROUTER_API_KEY"


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run a DeepSWE benchmark without destructive Pier retries."
    )
    parser.add_argument("gate", choices=("oracle", "canary", "full"))
    parser.add_argument(
        "--tasks", type=Path, default=Path("~/deep-swe/tasks"), help="DeepSWE tasks"
    )
    parser.add_argument("--clanker-binary", type=Path, default=None)
    parser.add_argument("--environment", choices=("modal", "docker"), default="modal")
    parser.add_argument("--concurrency", type=int, default=None)
    parser.add_argument("--model", default=None)
    parser.add_argument("--effort", default="high")
    parser.add_argument("--max-retries", type=int, default=DEFAULT_MAX_RETRIES)
    parser.add_argument(
        "--retry-max-delay-ms", type=int, default=DEFAULT_RETRY_MAX_DELAY_MS
    )
    parser.add_argument(
        "--confirm-full-sweep",
        action="store_true",
        help=f"acknowledge the estimated full-sweep cost ({FULL_SWEEP_COST})",
    )
    parser.add_argument(
        "--dry-run", action="store_true", help="print the Pier command without running it"
    )
    return parser


def _positive(parser: argparse.ArgumentParser, name: str, value: int) -> None:
    if value < 1:
        parser.error(f"{name} must be a positive integer")


def _bounded(
    parser: argparse.ArgumentParser,
    name: str,
    value: int,
    minimum: int,
    maximum: int,
) -> None:
    if value < minimum or value > maximum:
        parser.error(f"{name} must be between {minimum} and {maximum}")


def _binary_path(
    parser: argparse.ArgumentParser, requested: Path | None, dry_run: bool
) -> Path:
    raw = requested
    if raw is None and os.environ.get("CLANKER_BIN"):
        raw = Path(os.environ["CLANKER_BIN"])
    if raw is None:
        parser.error("--clanker-binary or CLANKER_BIN is required")
    binary = raw.expanduser().resolve()
    if not dry_run and not binary.is_file():
        parser.error(f"Clanker binary not found: {binary}")
    return binary


def build_command(
    gate: str,
    tasks: Path,
    environment: str,
    concurrency: int,
    binary: Path | None = None,
    model: str | None = None,
    effort: str = "high",
    max_retries: int = DEFAULT_MAX_RETRIES,
    retry_max_delay_ms: int = DEFAULT_RETRY_MAX_DELAY_MS,
    pier: str = "pier",
) -> list[str]:
    command = [
        pier,
        "run",
        "-p",
        str(tasks),
        "--env",
        environment,
        "-n",
        str(concurrency),
        "-r",
        "0",
    ]
    if gate == "oracle":
        return [*command, "--agent", "oracle"]

    if binary is None:
        raise ValueError("Clanker binary is required for agent benchmarks")
    selected_model = model or MODELS[gate]
    command.extend(
        [
            "--agent-import-path",
            "clanker_agent:ClankerAgent",
            "--model",
            selected_model,
            "--ak",
            f"clanker_binary={binary}",
            "--ak",
            f"effort={effort}",
            "--ak",
            f"max_retries={max_retries}",
            "--ak",
            f"retry_max_delay_ms={retry_max_delay_ms}",
            "-k",
            "1",
        ]
    )
    if gate == "canary":
        command.extend(["--n-tasks", "10", "--sample-seed", "0"])
    else:
        command.extend(["--agent-timeout-multiplier", "4"])
    return command


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    concurrency = args.concurrency or (8 if args.gate == "oracle" else DEFAULT_CONCURRENCY)
    _positive(parser, "--concurrency", concurrency)
    _bounded(parser, "--max-retries", args.max_retries, 0, MAX_RETRIES)
    _bounded(
        parser,
        "--retry-max-delay-ms",
        args.retry_max_delay_ms,
        1,
        MAX_U32,
    )

    if args.gate == "full" and not args.confirm_full_sweep:
        parser.error(
            f"the full sweep is estimated at {FULL_SWEEP_COST}; "
            "pass --confirm-full-sweep to acknowledge it"
        )

    tasks = args.tasks.expanduser().resolve()
    if not args.dry_run and not tasks.is_dir():
        parser.error(f"task directory not found: {tasks}")

    binary = None
    model = args.model
    if args.gate != "oracle":
        binary = _binary_path(parser, args.clanker_binary, args.dry_run)
        model = model or MODELS[args.gate]
        if not args.dry_run and not os.environ.get(PROVIDER_KEY):
            parser.error(f"{PROVIDER_KEY} is not set")

    pier = shutil.which("pier")
    if not pier and not args.dry_run:
        parser.error("pier is not installed or not on PATH")
    command = build_command(
        args.gate,
        tasks,
        args.environment,
        concurrency,
        binary,
        model,
        args.effort,
        args.max_retries,
        args.retry_max_delay_ms,
        pier or "pier",
    )

    print(shlex.join(command))
    if args.dry_run:
        return 0
    if args.gate == "full":
        print(f"WARNING: estimated cost is {FULL_SWEEP_COST}", file=sys.stderr)

    env = os.environ.copy()
    root = str(Path(__file__).resolve().parent)
    env["PYTHONPATH"] = root + os.pathsep + env.get("PYTHONPATH", "")
    return subprocess.run(command, cwd=root, env=env, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
