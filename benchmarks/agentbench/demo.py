"""`demo` -- agents side by side in tmux with a live memory readout."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile

SESSION = "agentbench"


def _tmux(*args: str, check: bool = False) -> subprocess.CompletedProcess:
    return subprocess.run(["tmux", *args], capture_output=True, text=True, check=check)


def _hold(command: str, name: str) -> str:
    """Keep a pane open after its program exits, so a crash is visible instead
    of collapsing the layout."""
    return f"{command}; printf '\\n[{name} exited - press a key]'; read -rsn1"


def _launch_command(tool) -> str:
    """Build the shell command for a pane, carrying the tool's full env.

    All of it, not just LD_LIBRARY_PATH: pi is a `#!/usr/bin/env node` script
    and silently fails to start if PATH does not reach the node install.
    """
    prefix = " ".join(f"{k}='{v}'" for k, v in sorted(tool.env.items()))
    parts = [prefix, f"'{tool.binary}'", *(f"'{a}'" for a in tool.args)]
    return " ".join(p for p in parts if p)


def main(args) -> int:
    tools = args.tools
    unavailable = [t for t in tools if not t.available()]
    if unavailable:
        for t in unavailable:
            print(f"!! {t.name} not found at {t.binary}")
        return 1

    if not shutil.which("tmux"):
        print("tmux is required: sudo apt install -y tmux", file=sys.stderr)
        return 1

    _tmux("kill-session", "-t", SESSION)
    workdir = tempfile.mkdtemp(prefix="agentbench-demo-")

    # The demo runs against the real HOME so each tool uses whatever credential
    # it already has. Deliberately no placeholder API key: a fake one is picked
    # up by every tool and makes each request fail.
    env = os.environ.copy()

    size = shutil.get_terminal_size(fallback=(200, 50))
    first, *rest = tools

    _tmux("new-session", "-d", "-s", SESSION, "-c", workdir,
          "-x", str(size.columns), "-y", str(size.lines),
          _hold(_launch_command(first), first.name))

    for tool in rest:
        _tmux("split-window", "-h", "-t", SESSION, "-c", workdir,
              _hold(_launch_command(tool), tool.name))

    _tmux("select-layout", "-t", SESSION, "even-horizontal")

    # Anchor to pane 0 rather than "the active pane", which may already have
    # exited and taken the layout with it.
    package_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    monitor = (f"cd '{package_root}' && {sys.executable} -m agentbench watch "
               f"--tools {' '.join(t.name for t in tools)}")
    _tmux("split-window", "-v", "-f", "-l", "12", "-t", f"{SESSION}:0.0",
          "-c", package_root, monitor)

    _tmux("set-option", "-t", SESSION, "-g", "pane-border-status", "top")
    _tmux("set-option", "-t", SESSION, "-g", "mouse", "on")
    _tmux("bind-key", "&", "kill-session")

    if args.detached:
        print(f"session '{SESSION}' started; attach with: tmux attach -t {SESSION}")
        return 0

    try:
        subprocess.run(["tmux", "attach", "-t", SESSION], env=env)
    finally:
        _tmux("kill-session", "-t", SESSION)
        shutil.rmtree(workdir, ignore_errors=True)
    return 0
