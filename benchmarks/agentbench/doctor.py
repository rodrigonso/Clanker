"""`doctor` -- check the environment before spending an hour on a benchmark."""

from __future__ import annotations

import os
import platform
import shutil
import subprocess
from pathlib import Path

from .measure import tool_version
from .tools import registry


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
    for name, tool in registry().items():
        if not tool.available():
            print(f"  {name:<10} MISSING at {tool.binary}")
            problems += 1
            continue
        real = tool.real_binary
        size_mb = os.path.getsize(real) / (1024 * 1024)
        print(f"  {name:<10} {tool_version(tool)}")
        print(f"  {'':<10} {real} ({size_mb:.1f} MB)")
        if tool.note:
            print(f"  {'':<10} note: {tool.note}")

    print("\n=== memory ===")
    try:
        for line in Path("/proc/meminfo").read_text().splitlines()[:3]:
            print(f"  {line}")
    except OSError:
        pass
    print("  10 concurrent opencode sessions need ~6 GB; run that one alone.")

    if problems:
        print(f"\n{problems} problem(s) found.")
        print("\nBuild clank for Linux with:")
        print("  cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release")
        print("  cmake --build build-linux --target clank")
        print("Install the others with:")
        print("  npm install -g @earendil-works/pi-coding-agent "
              "@anthropic-ai/claude-code opencode-ai")
        return 1

    print("\nAll good.")
    return 0
