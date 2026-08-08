#!/usr/bin/env python3
"""Process-tree PSS for interactive coding-agent CLIs.

A port of jcode's scripts/bench_memory_cli.py (from 1jehuang/jcode, MIT) with
Clank added as a measured tool. The measurement is deliberately unchanged from
the upstream script so the numbers line up with jcode's published table:

  * launch the real interactive TUI on a pty, answering the terminal capability
    queries a TUI probes for so it renders as it would for a user;
  * wait for the first meaningful line of screen content, type a short probe,
    and wait for that probe to echo back, which is what "ready for input" means
    here;
  * settle, then sum Pss across the whole process tree and process group, so a
    tool that spawns helpers or a daemon is charged for them.

Note what this does *not* do: the probe is typed but never submitted. No prompt
is ever sent, so this measures idle-at-ready footprint, not the cost of running
a turn. That is the same thing jcode's published RAM table measures.

PSS is Linux-only (/proc/<pid>/smaps_rollup); this does not run on Windows.
"""

from __future__ import annotations

import argparse
import json
import os
import pty
import re
import select
import shutil
import signal
import subprocess
import tempfile
import time
from dataclasses import asdict, dataclass
from pathlib import Path

ANSI_RE = re.compile(r"\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~]|\][^\x1b\x07]*(?:\x07|\x1b\\))")
PROBE = "jqx92"
DEFAULT_TIMEOUT_S = 25.0
DEFAULT_SETTLE_S = 1.0
DEFAULT_TOOLS = ["clank", "pi", "jcode_memory_off", "jcode_memory_on"]


@dataclass
class ToolSpec:
    name: str
    argv: list[str]
    version_argv: list[str]
    env: dict[str, str] | None = None


@dataclass
class SessionLaunch:
    root_pid: int
    pgid: int
    master_fd: int
    ready: bool
    input_ready: bool
    excerpt: str | None
    seconds_to_visible: float | None


@dataclass
class ToolRunResult:
    tool: str
    sessions: int
    pss_mb: float
    process_count: int
    version: str
    ready_sessions: int
    input_ready_sessions: int
    median_seconds_to_visible: float | None
    notes: list[str]


def build_specs() -> dict[str, ToolSpec]:
    home = Path.home()
    clank = os.environ.get("CLANK_BIN", str(home / "clank-src/build-linux/clank"))
    pi = os.environ.get("PI_BIN", str(home / ".local/npm/bin/pi"))
    jcode = os.environ.get("JCODE_BIN", str(home / ".local/bin/jcode"))

    # pi is a node script with a `/usr/bin/env node` shebang, and node here is a
    # local install rather than a system package.
    node_bin = str(home / ".local/node/bin")
    path = node_bin + os.pathsep + os.environ.get("PATH", "")

    # Clank links a locally staged OpenSSL, so the loader needs to find it.
    clank_env = {
        "PATH": path,
        "LD_LIBRARY_PATH": "/tmp/ssl/lib:" + os.environ.get("LD_LIBRARY_PATH", ""),
    }

    return {
        "clank": ToolSpec("clank", [clank], [clank, "--help"], env=clank_env),
        "pi": ToolSpec("pi", [pi], [pi, "--version"], env={"PATH": path}),
        # jcode's published table reports these two separately: its headline
        # figure is the embedding-off configuration, and its default (memory on)
        # is a different, much larger number.
        "jcode_memory_off": ToolSpec(
            "jcode_memory_off",
            [jcode, "--no-update", "--no-selfdev"],
            [jcode, "version"],
            env={"PATH": path, "JCODE_NO_TELEMETRY": "1", "JCODE_MEMORY_ENABLED": "0"},
        ),
        "jcode_memory_on": ToolSpec(
            "jcode_memory_on",
            [jcode, "--no-update", "--no-selfdev"],
            [jcode, "version"],
            env={"PATH": path, "JCODE_NO_TELEMETRY": "1", "JCODE_MEMORY_ENABLED": "1"},
        ),
    }


def reply_queries(master_fd: int, buffer: bytes) -> bytes:
    replies = [
        (b"\x1b[6n", b"\x1b[1;1R"),
        (b"\x1b[c", b"\x1b[?62;c"),
        (b"\x1b]10;?\x1b\\", b"\x1b]10;rgb:ffff/ffff/ffff\x1b\\"),
        (b"\x1b]11;?\x1b\\", b"\x1b]11;rgb:0000/0000/0000\x1b\\"),
        (b"\x1b]10;?\x07", b"\x1b]10;rgb:ffff/ffff/ffff\x07"),
        (b"\x1b]11;?\x07", b"\x1b]11;rgb:0000/0000/0000\x07"),
        (b"\x1b]4;0;?\x07", b"\x1b]4;0;rgb:0000/0000/0000\x07"),
        (b"\x1b[14t", b"\x1b[4;600;800t"),
        (b"\x1b[16t", b"\x1b[6;16;8t"),
        (b"\x1b[18t", b"\x1b[8;24;80t"),
        (b"\x1b[?1016$p", b"\x1b[?1016;1$y"),
        (b"\x1b[?2027$p", b"\x1b[?2027;1$y"),
        (b"\x1b[?2031$p", b"\x1b[?2031;1$y"),
        (b"\x1b[?1004$p", b"\x1b[?1004;1$y"),
        (b"\x1b[?2004$p", b"\x1b[?2004;1$y"),
        (b"\x1b[?2026$p", b"\x1b[?2026;1$y"),
    ]
    changed = True
    while changed:
        changed = False
        for query, response in replies:
            if query in buffer:
                os.write(master_fd, response)
                buffer = buffer.replace(query, b"")
                changed = True
    return buffer


def strip_ansi(text: str) -> str:
    return ANSI_RE.sub("", text).replace("\r", "\n")


def first_meaningful_line(text: str) -> str | None:
    for raw_line in text.splitlines():
        line = " ".join(raw_line.split())
        if not line:
            continue
        if sum(ch.isalnum() for ch in line) >= 3 and len(line) >= 4:
            return line[:160]
    return None


def launch_interactive(argv, cwd, env, timeout_s, settle_s) -> SessionLaunch:
    master_fd, slave_fd = pty.openpty()
    proc = subprocess.Popen(
        argv, cwd=str(cwd), env=env,
        stdin=slave_fd, stdout=slave_fd, stderr=slave_fd,
        preexec_fn=os.setsid,
    )
    os.close(slave_fd)
    os.set_blocking(master_fd, False)
    # setsid in the child makes pid == pgid; read it now rather than via
    # getpgid, which races a process that exits before we get there.
    pgid = proc.pid
    start = time.perf_counter()
    buf = b""
    ready = False
    input_ready = False
    probe_sent = False
    excerpt = None
    visible_at = None
    while time.perf_counter() - start < timeout_s:
        rlist, _, _ = select.select([master_fd], [], [], 0.05)
        if rlist:
            try:
                chunk = os.read(master_fd, 65536)
            except BlockingIOError:
                chunk = b""
            if chunk:
                buf += chunk
                buf = reply_queries(master_fd, buf)
                plain = strip_ansi(buf.decode("utf-8", "replace"))
                excerpt = first_meaningful_line(plain)
                if excerpt:
                    if not ready:
                        visible_at = time.perf_counter() - start
                    ready = True
                    if not probe_sent:
                        try:
                            os.write(master_fd, PROBE.encode())
                            probe_sent = True
                        except OSError:
                            break
                if probe_sent and PROBE in plain:
                    input_ready = True
                    break
        if proc.poll() is not None:
            break
    if input_ready or ready:
        time.sleep(settle_s)
    return SessionLaunch(
        root_pid=proc.pid, pgid=pgid, master_fd=master_fd,
        ready=ready, input_ready=input_ready, excerpt=excerpt,
        seconds_to_visible=visible_at,
    )


def iter_proc_stat() -> dict[int, tuple[int, int]]:
    out: dict[int, tuple[int, int]] = {}
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            stat = (entry / "stat").read_text()
            rest = stat[stat.rfind(")") + 2:].split()
            out[int(entry.name)] = (int(rest[1]), int(rest[2]))
        except Exception:
            continue
    return out


def collect_descendants(root_pids: list[int]) -> set[int]:
    ppid_of = iter_proc_stat()
    children: dict[int, list[int]] = {}
    for pid, (ppid, _) in ppid_of.items():
        children.setdefault(ppid, []).append(pid)
    seen: set[int] = set()
    stack = list(root_pids)
    while stack:
        pid = stack.pop()
        if pid in seen:
            continue
        seen.add(pid)
        stack.extend(children.get(pid, []))
    return seen


def collect_process_group_pids(pgids: list[int]) -> set[int]:
    wanted = set(pgids)
    return {pid for pid, (_, pgid) in iter_proc_stat().items() if pgid in wanted}


def read_pss_mb(pid: int) -> float | None:
    try:
        for line in Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines():
            if line.startswith("Pss:"):
                return int(line.split()[1]) / 1024.0
    except Exception:
        return None
    return None


def sum_tree_pss(root_pids: list[int], pgids: list[int]) -> tuple[float, int]:
    all_pids = collect_descendants(root_pids) | collect_process_group_pids(pgids)
    total, counted = 0.0, 0
    for pid in sorted(all_pids):
        pss = read_pss_mb(pid)
        if pss is None:
            continue
        total += pss
        counted += 1
    return round(total, 1), counted


def terminate_pgroup(pgid: int) -> None:
    for sig in (signal.SIGTERM, signal.SIGKILL):
        try:
            os.killpg(pgid, sig)
            time.sleep(0.2)
        except ProcessLookupError:
            return


def version_for(spec: ToolSpec) -> str:
    env = os.environ.copy()
    if spec.env:
        env.update(spec.env)
    proc = subprocess.run(spec.version_argv, capture_output=True, text=True, check=False, env=env)
    lines = (proc.stdout + proc.stderr).strip().splitlines()
    return lines[0] if lines else f"exit {proc.returncode}"


def wait_for_socket(path: str, timeout_s: float) -> bool:
    import socket as _socket
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if os.path.exists(path):
            try:
                sock = _socket.socket(_socket.AF_UNIX, _socket.SOCK_STREAM)
                sock.connect(path)
                sock.close()
                return True
            except OSError:
                pass
        time.sleep(0.05)
    return False


def run_tool(spec: ToolSpec, sessions: int, cwd: Path, timeout_s: float, settle_s: float) -> ToolRunResult:
    notes: list[str] = []
    version = version_for(spec)
    launches: list[SessionLaunch] = []
    cleanup_pgids: list[int] = []
    extra_roots: list[int] = []
    # Every tool gets a throwaway HOME. jcode's own script isolates only jcode
    # and lets the others inherit the real home directory; giving all three the
    # same treatment removes that asymmetry, and it also keeps a tool from being
    # charged for whatever sessions happen to be sitting in the user's profile.
    temp_root = tempfile.mkdtemp(prefix=f"cli-mem-{spec.name}-")
    argv = list(spec.argv)
    try:
        env = os.environ.copy()
        if spec.env:
            env.update(spec.env)
        temp_home = os.path.join(temp_root, "home")
        os.makedirs(temp_home, exist_ok=True)
        env["HOME"] = temp_home

        if spec.name == "pi":
            # pi downloads fd and ripgrep into ~/.pi on first run. Point the
            # fresh home at the already-populated cache so the measurement is
            # of a warm start, like it is for the other two.
            real_agent = Path.home() / ".pi" / "agent"
            if real_agent.exists():
                os.makedirs(os.path.join(temp_home, ".pi"), exist_ok=True)
                os.symlink(real_agent, os.path.join(temp_home, ".pi", "agent"))

        if spec.name.startswith("jcode"):
            # jcode is client/server: a session connects to a daemon that
            # survives the client. Start our own on a private socket so the
            # server's memory is attributed to this run instead of leaking into
            # (or being hidden by) a daemon left over from an earlier one.
            env["JCODE_HOME"] = os.path.join(temp_root, "jcode-home")
            env["JCODE_RUNTIME_DIR"] = os.path.join(temp_root, "run")
            env["JCODE_TEMP_SERVER"] = "1"
            env["JCODE_SERVER_OWNER_PID"] = str(os.getpid())
            # jcode refuses to start its server without a credential; clank and
            # pi both reach the input prompt without one. Nothing is ever sent
            # (the probe is typed, never submitted), so a placeholder only gets
            # jcode to the same starting line the other two reach unaided.
            env.setdefault("ANTHROPIC_API_KEY", "sk-ant-bench-placeholder-not-a-real-key")
            os.makedirs(env["JCODE_HOME"], exist_ok=True)
            os.makedirs(env["JCODE_RUNTIME_DIR"], exist_ok=True)
            socket_path = os.path.join(env["JCODE_RUNTIME_DIR"], "bench.sock")
            # --socket is a global flag, so it has to precede the subcommand.
            server = subprocess.Popen(
                [spec.argv[0], "--no-update", "--no-selfdev", "--socket", socket_path, "serve"],
                cwd=str(cwd), env=env,
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                preexec_fn=os.setsid,
            )
            extra_roots.append(server.pid)
            cleanup_pgids.append(server.pid)
            if not wait_for_socket(socket_path, timeout_s):
                notes.append("jcode server did not become ready")
            # Matches upstream: the embedding build does work after the socket
            # opens, so sampling too early catches it mid-initialisation and
            # reports a number that is not the steady state.
            if spec.name == "jcode_memory_on":
                time.sleep(max(settle_s, 5.0))
            argv = [spec.argv[0], "--no-update", "--no-selfdev", "--socket", socket_path]

        per_session_settle = max(settle_s, 2.0) if spec.name == "jcode_memory_on" else settle_s

        for _ in range(sessions):
            launches.append(launch_interactive(argv, cwd, env, timeout_s, per_session_settle))
            cleanup_pgids.append(launches[-1].pgid)
        root_pids = extra_roots + [launch.root_pid for launch in launches]

        for idx, launch in enumerate(launches, start=1):
            if not launch.ready:
                notes.append(f"session {idx}: no meaningful screen content before timeout")
            elif launch.excerpt:
                notes.append(f"session {idx}: {launch.excerpt}")
        pss_mb, process_count = sum_tree_pss(root_pids, cleanup_pgids)
        visible = sorted(l.seconds_to_visible for l in launches if l.seconds_to_visible is not None)
        return ToolRunResult(
            tool=spec.name, sessions=sessions, pss_mb=pss_mb, process_count=process_count,
            version=version,
            ready_sessions=sum(1 for l in launches if l.ready),
            input_ready_sessions=sum(1 for l in launches if l.input_ready),
            median_seconds_to_visible=(round(visible[len(visible) // 2], 3) if visible else None),
            notes=notes,
        )
    finally:
        for launch in launches:
            try:
                os.close(launch.master_fd)
            except Exception:
                pass
        for pgid in reversed(cleanup_pgids):
            terminate_pgroup(pgid)
        shutil.rmtree(temp_root, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Process-tree PSS for interactive agent CLIs")
    parser.add_argument("--sessions", type=int, required=True)
    parser.add_argument("--tools", nargs="*", default=DEFAULT_TOOLS)
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT_S)
    parser.add_argument("--settle", type=float, default=DEFAULT_SETTLE_S)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--cwd", default=os.getcwd())
    parser.add_argument("--json-out", default=None)
    args = parser.parse_args()

    specs = build_specs()
    cwd = Path(args.cwd).resolve()
    results = []
    for name in args.tools:
        spec = specs[name]
        if not Path(spec.argv[0]).exists() and not shutil.which(spec.argv[0]):
            print(f"!! {name}: binary not found at {spec.argv[0]}", flush=True)
            continue
        for attempt in range(args.repeat):
            label = f"{name} ({args.sessions} session{'s' if args.sessions != 1 else ''})"
            if args.repeat > 1:
                label += f" run {attempt + 1}/{args.repeat}"
            print(f"=== {label} ===", flush=True)
            result = run_tool(spec, args.sessions, cwd, args.timeout, args.settle)
            print(json.dumps(asdict(result), indent=2), flush=True)
            results.append(asdict(result))

    payload = {"cwd": str(cwd), "sessions": args.sessions, "results": results}
    if args.json_out:
        Path(args.json_out).write_text(json.dumps(payload, indent=2))

    # Median across repeats, because a tool that is still initialising when it
    # is sampled produces a high outlier rather than noise in both directions.
    print("\n=== summary (median PSS across repeats) ===", flush=True)
    print(f"{'tool':<20} {'sessions':>8} {'PSS MB':>9} {'procs':>6} {'runs':>5} {'all runs'}")
    by_tool: dict[str, list[dict]] = {}
    for r in results:
        by_tool.setdefault(r["tool"], []).append(r)
    for tool, runs in by_tool.items():
        vals = sorted(r["pss_mb"] for r in runs)
        med = vals[len(vals) // 2]
        procs = max(r["process_count"] for r in runs)
        ready = sum(r["input_ready_sessions"] for r in runs)
        want = sum(r["sessions"] for r in runs)
        flag = "" if ready == want else f"  (input-ready {ready}/{want})"
        print(f"{tool:<20} {args.sessions:>8} {med:>9.1f} {procs:>6} {len(runs):>5} "
              f"{', '.join(f'{v:.1f}' for v in vals)}{flag}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
