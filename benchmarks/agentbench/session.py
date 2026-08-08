"""Driving an interactive TUI on a pty.

A coding agent's TUI probes the terminal on startup and, in several cases,
blocks until it gets answers. Launching one on a bare pty and reading whatever
comes out measures a half-initialised program. This module answers the probes
a real terminal would, then establishes that the app is actually listening
before anything is sampled.
"""

from __future__ import annotations

import os
import pty
import re
import select
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path

ANSI_RE = re.compile(r"\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~]|\][^\x1b\x07]*(?:\x07|\x1b\\))")

# Typed to establish that the app is listening, never submitted. Deliberately
# not a word: it has to be recognisable in a screen dump full of box drawing.
PROBE = "jqx92"
PROBE_INTERVAL_S = 2.0

# CSI/OSC queries with fixed replies. Values are what a modest xterm answers.
_QUERIES = [
    (b"\x1b[6n", b"\x1b[1;1R"),                                    # cursor position
    (b"\x1b[c", b"\x1b[?62;c"),                                    # device attributes
    (b"\x1b]10;?\x1b\\", b"\x1b]10;rgb:ffff/ffff/ffff\x1b\\"),      # fg colour
    (b"\x1b]11;?\x1b\\", b"\x1b]11;rgb:0000/0000/0000\x1b\\"),      # bg colour
    (b"\x1b]10;?\x07", b"\x1b]10;rgb:ffff/ffff/ffff\x07"),
    (b"\x1b]11;?\x07", b"\x1b]11;rgb:0000/0000/0000\x07"),
    (b"\x1b]4;0;?\x07", b"\x1b]4;0;rgb:0000/0000/0000\x07"),        # palette
    (b"\x1b[14t", b"\x1b[4;600;800t"),                             # window px
    (b"\x1b[16t", b"\x1b[6;16;8t"),                                # cell px
    (b"\x1b[18t", b"\x1b[8;24;80t"),                               # window chars
    (b"\x1b[?1016$p", b"\x1b[?1016;1$y"),                          # SGR pixel mouse
    (b"\x1b[?2027$p", b"\x1b[?2027;1$y"),                          # grapheme clusters
    (b"\x1b[?2031$p", b"\x1b[?2031;1$y"),                          # colour scheme
    (b"\x1b[?1004$p", b"\x1b[?1004;1$y"),                          # focus reporting
    (b"\x1b[?2004$p", b"\x1b[?2004;1$y"),                          # bracketed paste
    (b"\x1b[?2026$p", b"\x1b[?2026;1$y"),                          # sync output
]

# Delimited queries, answered "unsupported". opencode waits on several of
# these, so leaving them unanswered strands it at a half-drawn screen.
_DELIMITED = [
    (b"\x1bP+q", b"\x1b\\", b"\x1bP0+q\x1b\\"),                    # XTGETTINCAP
    (b"\x1b_G", b"\x1b\\", b"\x1b_Gi=31337;ENOTSUP\x1b\\"),        # kitty graphics
    (b"\x1b]1337;", b"\x07", b""),                                 # iTerm2 caps
]


def answer_queries(fd: int, buf: bytes) -> bytes:
    changed = True
    while changed:
        changed = False
        for query, reply in _QUERIES:
            if query in buf:
                os.write(fd, reply)
                buf = buf.replace(query, b"")
                changed = True
        for prefix, terminator, reply in _DELIMITED:
            start = buf.find(prefix)
            if start < 0:
                continue
            end = buf.find(terminator, start + len(prefix))
            if end < 0:
                continue
            if reply:
                os.write(fd, reply)
            buf = buf[:start] + buf[end + len(terminator):]
            changed = True
    return buf


def strip_ansi(text: str) -> str:
    return ANSI_RE.sub("", text).replace("\r", "\n")


def first_meaningful_line(text: str) -> str | None:
    for raw in text.splitlines():
        line = " ".join(raw.split())
        if not line:
            continue
        if sum(c.isalnum() for c in line) >= 3 and len(line) >= 4:
            return line[:160]
    return None


@dataclass
class Session:
    pid: int
    pgid: int
    fd: int
    visible: bool           # painted something
    input_ready: bool       # echoed the probe back
    excerpt: str | None
    seconds_to_visible: float | None


def launch(argv: list[str], cwd: Path, env: dict[str, str],
           timeout_s: float, settle_s: float) -> Session:
    """Start a TUI on a pty and wait until it echoes a typed probe.

    Readiness is echo, not paint. A TUI can draw a full screen well before it
    is listening -- opencode in particular discards anything typed during
    startup -- so the probe is re-sent on an interval rather than once.
    """
    master, slave = pty.openpty()
    proc = subprocess.Popen(
        argv, cwd=str(cwd), env=env,
        stdin=slave, stdout=slave, stderr=slave,
        preexec_fn=os.setsid,
    )
    os.close(slave)
    os.set_blocking(master, False)
    # setsid makes pid == pgid; reading it now avoids racing a process that
    # exits before getpgid() is called.
    pgid = proc.pid

    start = time.perf_counter()
    buf = b""
    visible = False
    input_ready = False
    excerpt = None
    visible_at = None
    next_probe = 0.0

    while time.perf_counter() - start < timeout_s:
        readable, _, _ = select.select([master], [], [], 0.05)
        if readable:
            try:
                chunk = os.read(master, 65536)
            except (BlockingIOError, OSError):
                chunk = b""
            if chunk:
                buf += chunk
                buf = answer_queries(master, buf)
                plain = strip_ansi(buf.decode("utf-8", "replace"))
                excerpt = first_meaningful_line(plain) or excerpt
                if excerpt and not visible:
                    visible = True
                    visible_at = time.perf_counter() - start
                if PROBE in plain:
                    input_ready = True
                    break
        now = time.perf_counter()
        if visible and now >= next_probe:
            try:
                os.write(master, PROBE.encode())
            except OSError:
                break
            next_probe = now + PROBE_INTERVAL_S
        if proc.poll() is not None:
            break

    if visible:
        time.sleep(settle_s)

    return Session(
        pid=proc.pid, pgid=pgid, fd=master,
        visible=visible, input_ready=input_ready,
        excerpt=excerpt, seconds_to_visible=visible_at,
    )
