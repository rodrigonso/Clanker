"""macOS process-tree inspection: RSS accounting and ownership.

RSS includes shared pages in every process, so a tree total can double-count
shared libraries. It must not be compared directly with proportional memory
figures from other operating systems.
"""

from __future__ import annotations

import ctypes
import os
import signal
import socket
import subprocess
import sys
import time
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from .tools import Tool

_proc_pidpath = None
if sys.platform == "darwin":
    _libproc = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
    _proc_pidpath = _libproc.proc_pidpath
    _proc_pidpath.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32]
    _proc_pidpath.restype = ctypes.c_int

ProcessTable = dict[int, tuple[int, int, int]]


def proc_table() -> ProcessTable:
    """pid -> (ppid, pgid, RSS in KiB), sampled together with macOS ps."""
    result = subprocess.run(
        ["/bin/ps", "-axo", "pid=,ppid=,pgid=,rss="],
        capture_output=True, text=True, check=True,
    )
    out: ProcessTable = {}
    for line in result.stdout.splitlines():
        try:
            pid, ppid, pgid, rss = map(int, line.split())
        except ValueError:
            continue
        out[pid] = (ppid, pgid, rss)
    return out


def descendants(roots: list[int], table: ProcessTable) -> set[int]:
    children: dict[int, list[int]] = {}
    for pid, (ppid, _, _) in table.items():
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


def group_members(pgids: list[int], table: ProcessTable) -> set[int]:
    wanted = set(pgids)
    return {pid for pid, (_, pgid, _) in table.items() if pgid in wanted}


def tree_rss_mb(roots: list[int], pgids: list[int]) -> tuple[float, int]:
    """Include descendants and helpers reparented within the process group."""
    table = proc_table()
    pids = descendants(roots, table) | group_members(pgids, table)
    total = 0
    counted = 0
    for pid in pids:
        row = table.get(pid)
        if row and row[2] > 0:
            total += row[2]
            counted += 1
    return round(total / 1024.0, 1), counted


def read_exe(pid: int) -> str:
    if _proc_pidpath is None:
        return ""
    buffer = ctypes.create_string_buffer(4096)
    if _proc_pidpath(pid, buffer, len(buffer)) <= 0:
        return ""
    return os.path.realpath(os.fsdecode(buffer.value))


def read_cmdline(pid: int) -> str:
    try:
        result = subprocess.run(
            ["/bin/ps", "-ww", "-p", str(pid), "-o", "command="],
            capture_output=True, text=True, check=False,
        )
    except OSError:
        return ""
    return result.stdout.strip() if result.returncode == 0 else ""


def wait_for_socket(path: str, timeout_s: float) -> bool:
    """Wait for a Unix socket to accept connections, not merely exist."""
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
    """Attribute matching executable images to tools; return name -> (MiB, count).

    Live views use one entry per binary. Configuration variants are measured
    separately by their explicitly owned process trees in measure.
    """
    me = exclude_pid if exclude_pid is not None else os.getpid()
    totals = {t.name: 0 for t in tools}
    counts = {t.name: 0 for t in tools}
    reals = {t.name: t.real_binary for t in tools}

    for pid, (_, _, rss) in proc_table().items():
        if pid == me:
            continue
        exe = read_exe(pid)
        if not exe:
            continue
        cmd = read_cmdline(pid)
        for tool in tools:
            if tool.matches(pid, exe, cmd, reals[tool.name]):
                totals[tool.name] += rss
                counts[tool.name] += 1
                break
    return {n: (round(totals[n] / 1024.0, 1), counts[n]) for n in totals}


def terminate_group(pgid: int) -> None:
    for sig in (signal.SIGTERM, signal.SIGKILL):
        try:
            os.killpg(pgid, sig)
            time.sleep(0.2)
        except (ProcessLookupError, PermissionError):
            return
