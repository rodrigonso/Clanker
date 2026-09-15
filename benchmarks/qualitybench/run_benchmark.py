"""Validate external DeepSWE tasks with Pier's oracle, without running Clanker."""

from __future__ import annotations

import argparse
import shlex
import shutil
import subprocess
from pathlib import Path


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Validate DeepSWE tasks with Pier's oracle, not a Clanker agent."
    )
    parser.add_argument("gate", choices=("oracle",))
    parser.add_argument(
        "--tasks", type=Path, default=Path("~/deep-swe/tasks"), help="DeepSWE tasks"
    )
    parser.add_argument("--environment", choices=("modal", "docker"), default="modal")
    parser.add_argument("--concurrency", type=int, default=8)
    parser.add_argument(
        "--dry-run", action="store_true", help="print the Pier command without running it"
    )
    return parser


def build_command(
    tasks: Path,
    environment: str,
    concurrency: int,
    pier: str = "pier",
) -> list[str]:
    return [
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
        "--agent",
        "oracle",
    ]


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.concurrency < 1:
        parser.error("--concurrency must be a positive integer")

    tasks = args.tasks.expanduser().resolve()
    if not args.dry_run and not tasks.is_dir():
        parser.error(f"task directory not found: {tasks}")

    pier = shutil.which("pier")
    if not pier and not args.dry_run:
        parser.error("pier is not installed or not on PATH")
    command = build_command(
        tasks,
        args.environment,
        args.concurrency,
        pier or "pier",
    )

    print(shlex.join(command))
    if args.dry_run:
        return 0
    root = str(Path(__file__).resolve().parent)
    return subprocess.run(command, cwd=root, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
