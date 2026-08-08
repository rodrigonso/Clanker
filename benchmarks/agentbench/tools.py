"""Tool registry: the single source of truth for what gets measured.

Everything about a tool lives here -- where its binary is, how to launch it,
how to recognise its processes, and what first-run state it needs seeded. The
measure, watch and demo commands all read from this, so adding a tool is one
entry rather than three.
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

HOME = Path.home()


def _env_path(var: str, default: str) -> str:
    return os.environ.get(var, str(HOME / default))


@dataclass
class Tool:
    name: str
    binary: str
    args: list[str] = field(default_factory=list)
    version_args: list[str] = field(default_factory=lambda: ["--version"])
    env: dict[str, str] = field(default_factory=dict)
    # Given a resolved exe path, the full cmdline, and this tool's resolved
    # binary, is this process ours?
    matches: Callable[[str, str, str], bool] = lambda exe, cmd, real: exe == real
    # Seed whatever first-run state the tool needs to reach a real prompt.
    prepare: Callable[[str, Path], None] | None = None
    note: str = ""

    @property
    def real_binary(self) -> str:
        return os.path.realpath(self.binary)

    @property
    def argv(self) -> list[str]:
        return [self.binary, *self.args]

    def available(self) -> bool:
        return os.path.isfile(self.binary) and os.access(self.binary, os.X_OK)


def _node_script_match(script_name: str):
    """pi is a node script: exe is node and argv[0] is rewritten to a short
    name, so neither signal identifies it alone."""

    def match(exe: str, cmd: str, real: str) -> bool:
        if os.path.basename(exe) not in ("node", "nodejs"):
            return False
        first = os.path.basename(cmd.split()[0]) if cmd.split() else ""
        return first == script_name or real in cmd

    return match


def _prepare_claude(home: str, cwd: Path) -> None:
    """In a fresh HOME, Claude Code opens on a theme picker and then a
    folder-trust prompt. Sampling there measures an onboarding wizard rather
    than an agent session, so record both as already answered."""
    cfg = {
        "hasCompletedOnboarding": True,
        "theme": "dark",
        "numStartups": 5,
        "installMethod": "npm",
        "autoUpdates": False,
        "projects": {
            str(cwd): {
                "hasTrustDialogAccepted": True,
                "hasCompletedProjectOnboarding": True,
                "allowedTools": [],
                "history": [],
            }
        },
    }
    with open(os.path.join(home, ".claude.json"), "w") as f:
        json.dump(cfg, f)


def _prepare_pi(home: str, cwd: Path) -> None:
    """pi downloads fd and ripgrep into ~/.pi on first run. Point the fresh
    home at the already-populated cache so it is measured warm, like the
    others."""
    real_agent = HOME / ".pi" / "agent"
    if not real_agent.exists():
        return
    os.makedirs(os.path.join(home, ".pi"), exist_ok=True)
    link = os.path.join(home, ".pi", "agent")
    if not os.path.exists(link):
        os.symlink(real_agent, link)


def registry() -> dict[str, Tool]:
    node_bin = _env_path("NODE_BIN", ".local/node/bin")
    ssl_lib = _env_path("SSL_LIB", ".local/ssl/lib")
    path = node_bin + os.pathsep + os.environ.get("PATH", "")

    return {
        "clank": Tool(
            name="clank",
            binary=_env_path("CLANK_BIN", "clank-src/build-linux/clank"),
            version_args=["--help"],
            env={
                "PATH": path,
                # Clank links a locally staged OpenSSL; the loader needs it.
                "LD_LIBRARY_PATH": ssl_lib + os.pathsep + os.environ.get("LD_LIBRARY_PATH", ""),
            },
        ),
        "pi": Tool(
            name="pi",
            binary=_env_path("PI_BIN", ".local/npm/bin/pi"),
            env={"PATH": path},
            matches=_node_script_match("pi"),
            prepare=_prepare_pi,
        ),
        "claude": Tool(
            name="claude",
            binary=_env_path("CLAUDE_BIN", ".local/npm/bin/claude"),
            env={"PATH": path},
            prepare=_prepare_claude,
            note="native ELF despite the .exe name and npm install",
        ),
        "opencode": Tool(
            name="opencode",
            binary=_env_path("OPENCODE_BIN", ".local/npm/bin/opencode"),
            env={"PATH": path},
            note="native ELF despite the .exe name and npm install",
        ),
    }


DEFAULT_TOOLS = ["clank", "pi", "claude", "opencode"]


def resolve(names: list[str] | None) -> list[Tool]:
    """Look up tools by name, erroring on anything unknown rather than
    silently measuring a shorter list."""
    all_tools = registry()
    wanted = names or DEFAULT_TOOLS
    missing = [n for n in wanted if n not in all_tools]
    if missing:
        known = " ".join(sorted(all_tools))
        raise SystemExit(f"unknown tool(s): {' '.join(missing)}\nknown: {known}")
    return [all_tools[n] for n in wanted]
