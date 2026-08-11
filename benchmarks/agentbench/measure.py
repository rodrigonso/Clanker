"""`measure` -- the benchmark proper."""

from __future__ import annotations

import json
import os
import shutil
import statistics
import subprocess
import tempfile
from dataclasses import asdict, dataclass, field
from pathlib import Path

from . import proc, session
from .tools import SetupContext, Tool


@dataclass
class Run:
    tool: str
    sessions: int
    pss_mb: float
    process_count: int
    version: str
    visible_sessions: int
    input_ready_sessions: int
    median_seconds_to_visible: float | None
    notes: list[str] = field(default_factory=list)


def tool_version(tool: Tool) -> str:
    env = os.environ.copy()
    env.update(tool.env)
    try:
        r = subprocess.run([tool.binary, *tool.version_args],
                           capture_output=True, text=True, check=False, env=env, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return "unknown"
    lines = (r.stdout + r.stderr).strip().splitlines()
    return lines[0][:80] if lines else f"exit {r.returncode}"


def run_once(tool: Tool, sessions: int, cwd: Path,
             timeout_s: float, settle_s: float) -> Run:
    notes: list[str] = []
    version = tool_version(tool)
    launched: list[session.Session] = []
    pgids: list[int] = []
    daemon_pids: list[int] = []
    daemon_pgids: list[int] = []

    # Every tool gets a throwaway HOME, so none is charged for whatever
    # sessions and configuration happen to be in the real profile.
    root = tempfile.mkdtemp(prefix=f"agentbench-{tool.name}-")
    try:
        env = os.environ.copy()
        env.update(tool.env)
        home = os.path.join(root, "home")
        os.makedirs(home, exist_ok=True)
        env["HOME"] = home
        if tool.prepare:
            tool.prepare(home, cwd)

        argv = tool.argv
        settle = settle_s
        if tool.setup:
            plan = tool.setup(SetupContext(
                root=Path(root), home=Path(home), cwd=cwd, env=env,
                timeout_s=timeout_s, settle_s=settle_s,
            ))
            argv = plan.argv
            daemon_pids = plan.extra_pids
            daemon_pgids = plan.extra_pgids
            if plan.settle_s is not None:
                settle = max(settle_s, plan.settle_s)
                notes.append(f"settle raised to {settle:.1f}s for this tool")

        for _ in range(sessions):
            s = session.launch(argv, cwd, env, timeout_s, settle)
            launched.append(s)
            pgids.append(s.pgid)

        for i, s in enumerate(launched, start=1):
            if not s.visible:
                notes.append(f"session {i}: no screen content before timeout")
            elif s.excerpt:
                notes.append(f"session {i}: {s.excerpt}")

        pss_mb, count = proc.tree_pss_mb([s.pid for s in launched] + daemon_pids,
                                         pgids + daemon_pgids)
        # The failure this guards against is silent: a daemon that detaches
        # itself out of both the tree and the process group is simply not
        # charged, and the tool reports a number that is too good.
        expected = len(launched) + len(daemon_pids)
        if count < expected:
            notes.append(f"counted {count} processes, expected at least {expected}")
        seen = [s.seconds_to_visible for s in launched if s.seconds_to_visible is not None]

        return Run(
            tool=tool.name,
            sessions=sessions,
            pss_mb=pss_mb,
            process_count=count,
            version=version,
            visible_sessions=sum(1 for s in launched if s.visible),
            input_ready_sessions=sum(1 for s in launched if s.input_ready),
            median_seconds_to_visible=round(statistics.median(seen), 3) if seen else None,
            notes=notes,
        )
    finally:
        for s in launched:
            try:
                os.close(s.fd)
            except OSError:
                pass
        # Clients first, then the daemon they were talking to.
        for pgid in reversed(pgids):
            proc.terminate_group(pgid)
        for pgid in reversed(daemon_pgids):
            proc.terminate_group(pgid)
        shutil.rmtree(root, ignore_errors=True)


def summarise(runs: list[Run], sessions: int) -> None:
    by_tool: dict[str, list[Run]] = {}
    for r in runs:
        by_tool.setdefault(r.tool, []).append(r)

    print(f"\n=== median PSS across repeats, {sessions} "
          f"session{'s' if sessions != 1 else ''} ===")
    print(f"{'tool':<16} {'PSS MB':>9} {'ready':>7} {'procs':>6} {'to visible':>11}   all runs")
    for name, group in by_tool.items():
        values = sorted(r.pss_mb for r in group)
        median = values[len(values) // 2]
        procs = max(r.process_count for r in group)
        ready = sum(r.input_ready_sessions for r in group)
        want = sum(r.sessions for r in group)
        visible = [r.median_seconds_to_visible for r in group
                   if r.median_seconds_to_visible is not None]
        vis = f"{statistics.median(visible):.2f}s" if visible else "-"
        runs_txt = ", ".join(f"{v:.1f}" for v in values)
        print(f"{name:<16} {median:>9.1f} {ready:>3}/{want:<3} {procs:>6} {vis:>11}   {runs_txt}")
    if any(r.input_ready_sessions < r.sessions for r in runs):
        print("\n  'ready' counts sessions that echoed the probe. Below full means a\n"
              "  tool was still starting when sampled; raise --timeout or --settle.")


def main(args) -> int:
    tools = [t for t in args.tools]
    unavailable = [t for t in tools if not t.available()]
    if unavailable:
        for t in unavailable:
            print(f"!! {t.name} not found at {t.binary}")
        print("\nSet CLANK_BIN / JCODE_BIN / PI_BIN / CLAUDE_BIN / OPENCODE_BIN, or run "
              "`agentbench doctor`.")
        return 1

    cwd = Path(args.cwd).resolve()
    cwd.mkdir(parents=True, exist_ok=True)
    runs: list[Run] = []

    for tool in tools:
        for attempt in range(args.repeat):
            label = f"{tool.name} ({args.sessions} session{'s' if args.sessions != 1 else ''})"
            if args.repeat > 1:
                label += f" run {attempt + 1}/{args.repeat}"
            print(f"=== {label} ===", flush=True)
            try:
                run = run_once(tool, args.sessions, cwd, args.timeout, args.settle)
            except RuntimeError as err:
                # A tool whose daemon never came up has no number worth
                # reporting, but the rest of the table is still good.
                print(f"    !! {err}\n    skipping {tool.name}", flush=True)
                break
            print(f"    {run.pss_mb:.1f} MB   {run.process_count} proc   "
                  f"ready {run.input_ready_sessions}/{run.sessions}", flush=True)
            runs.append(run)

    if not runs:
        print("\nno tool produced a measurement")
        return 1

    summarise(runs, args.sessions)

    if args.json_out:
        payload = {"cwd": str(cwd), "sessions": args.sessions,
                   "results": [asdict(r) for r in runs]}
        Path(args.json_out).write_text(json.dumps(payload, indent=2))
        print(f"\nwrote {args.json_out}")
    return 0
