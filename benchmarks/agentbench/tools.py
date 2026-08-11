"""Tool registry: the single source of truth for what gets measured.

Everything about a tool lives here -- where its binary is, how to launch it,
how to recognise its processes, and what first-run state it needs seeded. The
measure, watch and demo commands all read from this, so adding a tool is one
entry rather than three.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

from .proc import read_environ, terminate_group, wait_for_socket

HOME = Path.home()


def _env_path(var: str, default: str) -> str:
    return os.environ.get(var, str(HOME / default))


@dataclass
class SetupContext:
    """What a setup hook is given: the run's scratch space and environment.

    `env` is mutable and is what every session inherits, so a hook that needs
    to point a tool at a private socket or config directory writes it here.
    """
    root: Path              # throwaway per-run root, removed afterwards
    home: Path              # HOME for this run
    cwd: Path               # where the agents are launched
    env: dict[str, str]
    timeout_s: float
    settle_s: float


@dataclass
class Launch:
    """What a setup hook contributes beyond the session processes.

    A tool that serves its sessions from a separate daemon has to declare that
    daemon: it is charged for in the PSS total and torn down with the run.
    """
    argv: list[str]
    extra_pids: list[int] = field(default_factory=list)
    extra_pgids: list[int] = field(default_factory=list)
    settle_s: float | None = None


@dataclass
class Tool:
    name: str
    binary: str
    args: list[str] = field(default_factory=list)
    version_args: list[str] = field(default_factory=lambda: ["--version"])
    env: dict[str, str] = field(default_factory=dict)
    # Given a pid, its resolved exe path, the full cmdline, and this tool's
    # resolved binary, is this process ours?
    matches: Callable[[int, str, str, str], bool] = lambda pid, exe, cmd, real: exe == real
    # Seed whatever first-run state the tool needs to reach a real prompt.
    prepare: Callable[[str, Path], None] | None = None
    # Start whatever has to exist before the sessions do, and say how to launch
    # them. Only tools with a daemon need this.
    setup: Callable[[SetupContext], Launch] | None = None
    # Some tools ship a launcher that execs the real image beside it. Matching
    # a running process and sizing the install both have to follow that hop.
    image: Callable[[str], str] | None = None
    note: str = ""

    @property
    def real_binary(self) -> str:
        resolved = os.path.realpath(self.binary)
        return self.image(resolved) if self.image else resolved

    @property
    def argv(self) -> list[str]:
        return [self.binary, *self.args]

    def available(self) -> bool:
        return os.path.isfile(self.binary) and os.access(self.binary, os.X_OK)


def _node_script_match(script_name: str):
    """pi is a node script: exe is node and argv[0] is rewritten to a short
    name, so neither signal identifies it alone."""

    def match(pid: int, exe: str, cmd: str, real: str) -> bool:
        if os.path.basename(exe) not in ("node", "nodejs"):
            return False
        first = os.path.basename(cmd.split()[0]) if cmd.split() else ""
        return first == script_name or real in cmd

    return match


def _jcode_image(resolved: str) -> str:
    """`jcode` on disk is a small sh launcher that sets LD_LIBRARY_PATH and
    execs the platform image beside it, so the launcher's path is neither what
    a running process reports as its exe nor what the install actually weighs.
    """
    directory = os.path.dirname(resolved)
    try:
        names = sorted(os.listdir(directory))
    except OSError:
        return resolved
    for name in names:
        if name.startswith("jcode-") and name.endswith(".bin"):
            return os.path.join(directory, name)
    return resolved


def _jcode_match(memory_enabled: bool):
    """Both jcode variants are the same image -- and so is the daemon serving
    them -- so the resolved path cannot tell them apart. The environment can.

    An unlabelled jcode, such as one the user is really running, is a
    default-configuration jcode, which is what the memory-on variant is.
    """
    want = f"JCODE_MEMORY_ENABLED={'1' if memory_enabled else '0'}"
    other = f"JCODE_MEMORY_ENABLED={'0' if memory_enabled else '1'}"

    def match(pid: int, exe: str, cmd: str, real: str) -> bool:
        if exe != real:
            return False
        environ = read_environ(pid)
        if want in environ:
            return True
        return memory_enabled and other not in environ

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


# Global flags, before any subcommand. --no-update stops jcode self-updating
# mid-benchmark and changing the version under measurement; --no-selfdev only
# bites inside the jcode repo but costs nothing here.
_JCODE_FLAGS = ["--no-update", "--no-selfdev"]

# Taken warm from the real profile so jcode opens on a prompt rather than a
# login screen -- the same exception _prepare_pi already makes for pi's cache.
# Copied, not symlinked: jcode rejects auth files that are symlinks (its own
# scripts/onboarding_sandbox.sh says so, and copies for the same reason).
_JCODE_AUTH = ("anthropic-auth.json", "openai-auth.json", "antigravity_oauth.json",
               "gemini_oauth.json", "config.toml")


def _copy_if_present(src: Path, dst: Path) -> None:
    if src.exists() and not dst.exists():
        shutil.copyfile(src, dst)
        dst.chmod(0o600)


def _link_if_present(src: Path, dst: Path) -> None:
    if src.exists() and not dst.exists():
        os.symlink(src, dst)


def _setup_jcode(memory_enabled: bool):
    """jcode serves every session from one daemon, so the benchmark starts that
    daemon itself.

    Letting jcode spawn its own would measure the wrong thing three ways: the
    daemon is `setsid()`-detached, so it is in neither the client's descendant
    tree nor its process group and would go uncounted entirely; it outlives the
    run on a five-minute idle timeout, so the next repeat would find it warm;
    and its socket lives outside HOME, so the throwaway home does not stop it
    colliding with a jcode the user is really running.

    This mirrors jcode's own scripts/bench_memory_cli.py, which is what makes
    the numbers comparable to the table jcode publishes.
    """

    def setup(ctx: SetupContext) -> Launch:
        jcode_home = ctx.root / "jcode-home"
        runtime = ctx.root / "run"
        jcode_home.mkdir(parents=True, exist_ok=True)
        runtime.mkdir(parents=True, exist_ok=True)
        socket_path = runtime / "bench.sock"

        ctx.env.update({
            "JCODE_HOME": str(jcode_home),
            "JCODE_RUNTIME_DIR": str(runtime),
            "JCODE_TEMP_SERVER": "1",
            # Ties the daemon's lifetime to the benchmark's, so a crash between
            # here and teardown cannot leave it running against the next repeat.
            "JCODE_SERVER_OWNER_PID": str(os.getpid()),
            "JCODE_THEME": "dark",
        })
        # JCODE_MEMORY_ENABLED is not set here: it comes from the entry's static
        # env, so demo and watch panes carry it too and can be told apart.

        real = HOME / ".jcode"
        for name in _JCODE_AUTH:
            _copy_if_present(real / name, jcode_home / name)
        if memory_enabled:
            # The embedding model is the whole difference between the two
            # variants; linking it measures it warm rather than mid-download.
            _link_if_present(real / "models", jcode_home / "models")

        # jcode's server exits rather than start without a credential. Nothing
        # is ever sent -- measure types its probe and never submits it -- so a
        # placeholder only gets past the gate, and it is what jcode's own idle
        # profiling scripts use. A real credential, linked above or already in
        # the environment, is left alone.
        ctx.env.setdefault("ANTHROPIC_API_KEY", "sk-ant-agentbench-placeholder")

        binary = _env_path("JCODE_BIN", ".local/bin/jcode")
        server = subprocess.Popen(
            [binary, *_JCODE_FLAGS, "serve", "--socket", str(socket_path)],
            cwd=str(ctx.cwd), env=ctx.env,
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            preexec_fn=os.setsid,
        )
        # setsid makes pid == pgid, as in session.launch.
        if not wait_for_socket(str(socket_path), ctx.timeout_s):
            terminate_group(server.pid)
            raise RuntimeError(f"jcode server did not accept on {socket_path} "
                               f"within {ctx.timeout_s:.0f}s")

        return Launch(
            argv=[binary, *_JCODE_FLAGS, "--socket", str(socket_path)],
            extra_pids=[server.pid],
            extra_pgids=[server.pid],
            # With the embedding model on, the server is still loading it well
            # past the usual one second and samples low.
            settle_s=5.0 if memory_enabled else None,
        )

    return setup


def registry() -> dict[str, Tool]:
    node_bin = _env_path("NODE_BIN", ".local/node/bin")
    ssl_lib = _env_path("SSL_LIB", ".local/ssl/lib")
    path = node_bin + os.pathsep + os.environ.get("PATH", "")

    jcode_bin = _env_path("JCODE_BIN", ".local/bin/jcode")

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
        "jcode": Tool(
            name="jcode",
            binary=jcode_bin,
            args=_JCODE_FLAGS,
            version_args=["version"],
            env={"PATH": path, "JCODE_NO_TELEMETRY": "1", "JCODE_MEMORY_ENABLED": "1"},
            matches=_jcode_match(True),
            setup=_setup_jcode(True),
            image=_jcode_image,
            note="client/server: measured with its own daemon, in its default "
                 "configuration",
        ),
        "jcode-embed-off": Tool(
            name="jcode-embed-off",
            binary=jcode_bin,
            args=_JCODE_FLAGS,
            version_args=["version"],
            env={"PATH": path, "JCODE_NO_TELEMETRY": "1", "JCODE_MEMORY_ENABLED": "0"},
            matches=_jcode_match(False),
            setup=_setup_jcode(False),
            image=_jcode_image,
            note="same image as jcode, with the local embedding model off",
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


DEFAULT_TOOLS = ["clank", "jcode", "jcode-embed-off", "pi", "claude", "opencode"]

# watch and demo attribute processes to tools live, and every jcode session on
# a machine is served by one daemon. Two jcode panes would share it and both
# would be misread, so the live views carry a single jcode.
LIVE_TOOLS = ["clank", "jcode", "pi", "claude", "opencode"]


def resolve(names: list[str] | None, default: list[str] | None = None) -> list[Tool]:
    """Look up tools by name, erroring on anything unknown rather than
    silently measuring a shorter list."""
    all_tools = registry()
    wanted = names or default or DEFAULT_TOOLS
    missing = [n for n in wanted if n not in all_tools]
    if missing:
        known = " ".join(sorted(all_tools))
        raise SystemExit(f"unknown tool(s): {' '.join(missing)}\nknown: {known}")
    return [all_tools[n] for n in wanted]
