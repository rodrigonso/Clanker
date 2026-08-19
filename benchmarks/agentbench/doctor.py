"""`doctor` -- check the environment before spending an hour on a benchmark."""

from __future__ import annotations

import os
import platform
import shutil
import subprocess
from pathlib import Path

from . import proc
from .measure import tool_version
from .tools import registry


def _jcode_servers(binary: str) -> list[int]:
    """Servers already running for this user.

    A leftover jcode server is the likeliest source of a bad number: it holds
    the shared socket, so the next measurement either attaches to it or fights
    it for the path.
    """
    real = os.path.realpath(binary)
    found = []
    for name in os.listdir("/proc"):
        if not name.isdigit():
            continue
        pid = int(name)
        if proc.read_exe(pid) == real and "serve" in proc.read_cmdline(pid).split():
            found.append(pid)
    return found


def main(args) -> int:
    problems = 0

    print("=== platform ===")
    print(f"  {platform.platform()}")
    if platform.system() != "Linux":
        print("  !! PSS is Linux-only; run this under WSL on Windows")
        problems += 1
    rollup = Path("/proc/self/smaps_rollup")
    print(f"  /proc/self/smaps_rollup: {'ok' if rollup.exists() else 'MISSING'}")
    if not rollup.exists():
        problems += 1

    print("\n=== required commands ===")
    for cmd, why in (("tmux", "demo only"), ("python3", "everything")):
        found = shutil.which(cmd)
        print(f"  {cmd:<8} {found or 'MISSING (' + why + ')'}")
        if not found and cmd == "python3":
            problems += 1

    print("\n=== tools ===")
    seen: dict[str, str] = {}
    for name, tool in registry().items():
        if not tool.available():
            print(f"  {name:<16} MISSING at {tool.binary}")
            problems += 1
            continue
        real = tool.real_binary
        if real in seen:
            # Two variants of one tool: report it once, not twice.
            print(f"  {name:<16} same image as {seen[real]}")
        else:
            seen[real] = name
            size_mb = os.path.getsize(real) / (1024 * 1024)
            print(f"  {name:<16} {tool_version(tool)}")
            print(f"  {'':<16} {real} ({size_mb:.1f} MB)")
        if tool.note:
            print(f"  {'':<16} note: {tool.note}")

    jcode = registry().get("jcode")
    if jcode and jcode.available():
        stale = _jcode_servers(jcode.binary)
        if stale:
            print(f"\n  !! jcode server already running (pid {', '.join(map(str, stale))}).")
            print("     Each measurement starts its own on a private socket, but a "
                  "server\n     left from a real session can still skew a live "
                  "`watch`. Stop it first.")

    print("\n=== memory ===")
    try:
        for line in Path("/proc/meminfo").read_text().splitlines()[:3]:
            print(f"  {line}")
    except OSError:
        pass
    print("  10 concurrent opencode sessions need ~6 GB; run that one alone.")

    if problems:
        print(f"\n{problems} problem(s) found.")
        print("\nBuild clanker for Linux with:")
        print("  cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release")
        print("  cmake --build build-linux --target clanker")
        print("Install jcode with:")
        print("  curl -fsSL https://jcode.sh/install | bash")
        print("Install the others with:")
        print("  npm install -g @earendil-works/pi-coding-agent "
              "@anthropic-ai/claude-code opencode-ai")
        return 1

    print("\nAll good.")
    return 0
