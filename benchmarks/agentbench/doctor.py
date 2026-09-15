"""`doctor` -- check the environment before spending an hour on a benchmark."""

from __future__ import annotations

import os
import platform
import shutil
import subprocess

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
    for pid in proc.proc_table():
        if proc.read_exe(pid) == real and "serve" in proc.read_cmdline(pid).split():
            found.append(pid)
    return found


def main(args) -> int:
    problems = 0

    print("=== platform ===")
    print(f"  {platform.platform()}")
    if platform.system() != "Darwin":
        print("  !! The interactive comparison requires macOS")
        return 1
    try:
        table = proc.proc_table()
        own_rss = table.get(os.getpid(), (0, 0, 0))[2]
        print(f"  process RSS: {'ok' if own_rss else 'UNAVAILABLE'}")
        if not own_rss:
            problems += 1
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"  !! process inspection failed: {error}")
        return 1
    if not proc.read_exe(os.getpid()):
        print("  !! libproc cannot resolve executable paths")
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
        stale = _jcode_servers(jcode.real_binary)
        if stale:
            print(f"\n  !! jcode server already running (pid {', '.join(map(str, stale))}).")
            print("     Each measurement starts its own on a private socket, but a "
                  "server\n     left from a real session can still skew a live "
                  "`watch`. Stop it first.")

    print("\n=== memory ===")
    try:
        memory = subprocess.run(["/usr/sbin/sysctl", "-n", "hw.memsize"],
                                capture_output=True, text=True, check=True)
        print(f"  physical memory: {int(memory.stdout.strip()) / (1024 ** 3):.1f} GiB")
    except (OSError, ValueError, subprocess.CalledProcessError):
        print("  physical memory: unavailable")
    print("  RSS counts shared pages in each process; concurrent sessions can be costly.")

    if problems:
        print(f"\n{problems} problem(s) found.")
        print("\nBuild clanker for macOS from the repository root with:")
        print("  bash tools/build.sh --config Release")
        print("Install jcode with:")
        print("  curl -fsSL https://jcode.sh/install | bash")
        print("Install the others with:")
        print("  npm install -g @earendil-works/pi-coding-agent "
              "@anthropic-ai/claude-code opencode-ai")
        return 1

    print("\nAll good.")
    return 0
