"""Process-tree inspection: PSS accounting and ownership.

PSS (Proportional Set Size) divides each shared page by the number of
processes mapping it, so summing it across a tree gives a total that does not
double-count a shared library. Linux only -- there is no Windows analogue.
"""

from __future__ import annotations

import os
import signal
import socket
import time
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:  # importing Tool at runtime would be circular: tools.py
    from .tools import Tool  # needs terminate_group and wait_for_socket.


def pss_kb(pid: int) -> int:
    try:
        with open(f"/proc/{pid}/smaps_rollup") as f:
            for line in f:
                if line.startswith("Pss:"):
                    return int(line.split()[1])
    except OSError:
        pass
    return 0


def pss_breakdown(pid: int) -> dict[str, int]:
    """Per-mapping PSS in KB, keyed by backing file. Used to explain a total
    rather than just report it -- e.g. how much of Clank is really OpenSSL."""
    out: dict[str, int] = {}
    current = ""
    try:
        with open(f"/proc/{pid}/smaps") as f:
            for line in f:
                if not line.startswith(" ") and "-" in line.split()[0]:
                    parts = line.rstrip().split()
                    current = parts[-1] if len(parts) > 5 else "[anon]"
                elif line.startswith("Pss:"):
                    out[current] = out.get(current, 0) + int(line.split()[1])
    except OSError:
        pass
    return out


def proc_table() -> dict[int, tuple[int, int]]:
    """pid -> (ppid, pgid) for every readable process."""
    out: dict[int, tuple[int, int]] = {}
    for name in os.listdir("/proc"):
        if not name.isdigit():
            continue
        try:
            stat = Path(f"/proc/{name}/stat").read_text()
            rest = stat[stat.rfind(")") + 2:].split()
            out[int(name)] = (int(rest[1]), int(rest[2]))
        except (OSError, ValueError, IndexError):
            continue
    return out


def descendants(roots: list[int]) -> set[int]:
    table = proc_table()
    children: dict[int, list[int]] = {}
    for pid, (ppid, _) in table.items():
        children.setdefault(ppid, []).append(pid)
    seen: set[int] = set()
    stack = list(roots)
    while stack:
        pid = stack.pop()
        if pid in seen:
            continue
        seen.add(pid)
        stack.extend(children.get(pid, []))
    return seen


def group_members(pgids: list[int]) -> set[int]:
    wanted = set(pgids)
    return {pid for pid, (_, pgid) in proc_table().items() if pgid in wanted}


def tree_pss_mb(roots: list[int], pgids: list[int]) -> tuple[float, int]:
    """Total PSS over the process tree and process group. Both are needed: a
    tool may reparent a helper away from its launcher, or spawn a daemon that
    is not a child at all."""
    pids = descendants(roots) | group_members(pgids)
    total = 0
    counted = 0
    for pid in sorted(pids):
        kb = pss_kb(pid)
        if kb:
            total += kb
            counted += 1
    return round(total / 1024.0, 1), counted


def read_exe(pid: int) -> str:
    try:
        return os.readlink(f"/proc/{pid}/exe")
    except OSError:
        return ""


def read_cmdline(pid: int) -> str:
    try:
        with open(f"/proc/{pid}/cmdline", "rb") as f:
            return f.read().decode("utf-8", "replace").replace("\0", " ").strip()
    except OSError:
        return ""


def read_environ(pid: int) -> set[str]:
    """Environment of a running process, as `K=V` entries.

    Two tools can be the same binary and differ only in how they were
    configured, which neither the exe path nor the cmdline reveals.
    """
    try:
        with open(f"/proc/{pid}/environ", "rb") as f:
            raw = f.read().decode("utf-8", "replace")
    except OSError:
        return set()
    return {entry for entry in raw.split("\0") if entry}


def wait_for_socket(path: str, timeout_s: float) -> bool:
    """Wait until a Unix socket is accepting connections.

    Existence is not readiness: the server binds the path before it listens, so
    a launcher that only waits for the file races the first client.
    """
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if os.path.exists(path):
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                sock.connect(path)
                return True
            except OSError:
                pass
            finally:
                sock.close()
        time.sleep(0.05)
    return False


def scan(tools: list["Tool"], exclude_pid: int | None = None) -> dict[str, tuple[float, int]]:
    """Attribute every running process to a tool. Returns name -> (MB, count).

    Matching is on the resolved binary rather than a name substring, so the
    scanner never counts itself or a shell that merely mentions the tool.
    """
    me = exclude_pid if exclude_pid is not None else os.getpid()
    totals = {t.name: 0.0 for t in tools}
    counts = {t.name: 0 for t in tools}
    reals = {t.name: t.real_binary for t in tools}

    for name in os.listdir("/proc"):
        if not name.isdigit():
            continue
        pid = int(name)
        if pid == me:
            continue
        exe = read_exe(pid)
        if not exe:
            continue
        cmd = read_cmdline(pid)
        for tool in tools:
            if tool.matches(pid, exe, cmd, reals[tool.name]):
                totals[tool.name] += pss_kb(pid) / 1024.0
                counts[tool.name] += 1
                break
    return {n: (round(totals[n], 1), counts[n]) for n in totals}


def terminate_group(pgid: int) -> None:
    for sig in (signal.SIGTERM, signal.SIGKILL):
        try:
            os.killpg(pgid, sig)
            time.sleep(0.2)
        except (ProcessLookupError, PermissionError):
            return
