#!/usr/bin/env bash
# Launch clank, pi and jcode side by side with a live memory readout.
#
#   ./demo.sh            three agents in tmux + a PSS monitor along the bottom
#   ./demo.sh --monitor  just the monitor, against whatever is already running
#
# Memory is Pss from /proc/<pid>/smaps_rollup, summed per tool over every
# process belonging to it -- which for jcode includes the background daemon it
# spawns, since that is part of what a session costs.

set -uo pipefail

SESSION=agents
SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"

CLANK_BIN="${CLANK_BIN:-$HOME/clank-src/build-clean/clank}"
PI_BIN="${PI_BIN:-$HOME/.local/npm/bin/pi}"
JCODE_BIN="${JCODE_BIN:-$HOME/.local/bin/jcode}"
SSL_LIB="${SSL_LIB:-$HOME/.local/ssl/lib}"
NODE_BIN="${NODE_BIN:-$HOME/.local/node/bin}"

# ----------------------------------------------------------------- monitor --
if [[ "${1:-}" == "--monitor" ]]; then
  exec python3 - "$CLANK_BIN" "$PI_BIN" "$JCODE_BIN" <<'PY'
import os, sys, time

CLANK, PI, JCODE = sys.argv[1], sys.argv[2], sys.argv[3]
CLANK_REAL = os.path.realpath(CLANK)
JCODE_REAL = os.path.realpath(JCODE)
ME = os.getpid()

def pss_kb(pid):
    try:
        with open(f"/proc/{pid}/smaps_rollup") as f:
            for line in f:
                if line.startswith("Pss:"):
                    return int(line.split()[1])
    except Exception:
        pass
    return 0

def procs():
    for name in os.listdir("/proc"):
        if not name.isdigit():
            continue
        pid = int(name)
        if pid == ME:
            continue
        try:
            exe = os.readlink(f"/proc/{pid}/exe")
        except Exception:
            exe = ""
        try:
            with open(f"/proc/{pid}/cmdline", "rb") as f:
                cmd = f.read().decode("utf-8", "replace").replace("\0", " ").strip()
        except Exception:
            cmd = ""
        yield pid, exe, cmd

def classify(exe, cmd):
    # Match on the resolved binary rather than a loose name, so the monitor
    # never counts itself or a shell that merely mentions the tool.
    if exe and exe == CLANK_REAL:
        return "clank"
    # ~/.local/bin/jcode is a shim that execs a versioned binary out of
    # ~/.jcode/builds, and the daemon it spawns is that same binary, so match
    # the install tree instead of the launcher path.
    if exe and (exe == JCODE_REAL or "/.jcode/builds/" in exe or
                os.path.basename(exe).startswith("jcode-")):
        return "jcode"
    # pi is a node script: its exe is node and its argv is rewritten to "pi",
    # so neither alone is enough -- take node processes that say they are pi.
    if exe and os.path.basename(exe) in ("node", "nodejs"):
        first = cmd.split()[0] if cmd.split() else ""
        if os.path.basename(first) == "pi" or "pi-coding-agent" in cmd or PI in cmd:
            return "pi"
    return None

TOOLS = ["clank", "pi", "jcode"]
COLOR = {"clank": "\033[92m", "pi": "\033[93m", "jcode": "\033[96m"}
RESET = "\033[0m"

peak = {t: 0.0 for t in TOOLS}
try:
    while True:
        tot = {t: 0.0 for t in TOOLS}
        cnt = {t: 0 for t in TOOLS}
        for pid, exe, cmd in procs():
            tool = classify(exe, cmd)
            if tool:
                tot[tool] += pss_kb(pid) / 1024.0
                cnt[tool] += 1
        for t in TOOLS:
            peak[t] = max(peak[t], tot[t])

        live = [tot[t] for t in TOOLS if cnt[t]]
        scale = max(live) if live else 1.0
        base = min((v for v in live if v > 0), default=0.0)

        out = ["\033[H\033[J", "  live memory  (PSS, whole process tree)\n"]
        for t in TOOLS:
            if not cnt[t]:
                out.append(f"  {t:<7} {'--':>9}    (not running)\n")
                continue
            width = int(38 * tot[t] / scale) if scale else 0
            rel = f"{tot[t]/base:.0f}x" if base and tot[t] > base else ""
            out.append(
                f"  {COLOR[t]}{t:<7}{RESET} {tot[t]:>7.1f} MB  "
                f"{COLOR[t]}{'█' * width}{RESET}{' ' * (38 - width)} "
                f"{cnt[t]} proc  peak {peak[t]:.1f}  {rel}\n"
            )
        out.append("\n  Ctrl-b d detaches   Ctrl-b & kills the demo\n")
        sys.stdout.write("".join(out))
        sys.stdout.flush()
        time.sleep(1)
except KeyboardInterrupt:
    pass
PY
fi

# ------------------------------------------------------------------- checks --
missing=0
for pair in "clank:$CLANK_BIN" "pi:$PI_BIN" "jcode:$JCODE_BIN"; do
  name="${pair%%:*}"; path="${pair#*:}"
  if [[ ! -x "$path" ]]; then
    echo "!! $name not found at $path" >&2
    missing=1
  fi
done
if [[ $missing -eq 1 ]]; then
  echo >&2
  echo "Set CLANK_BIN / PI_BIN / JCODE_BIN, or build clank with:" >&2
  echo "  cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release" >&2
  echo "  cmake --build build-linux --target clank" >&2
  exit 1
fi

if ! command -v tmux >/dev/null 2>&1; then
  echo "tmux is required: sudo apt install -y tmux" >&2
  exit 1
fi

# jcode leaves a daemon running that the next launch reuses. Left alone it
# makes a second demo look cheaper than the first, because the server is
# already up and nobody pays for it. The launcher is a shim, so kill the
# versioned binary it execs rather than the shim path.
kill_jcode() { pkill -f '/\.jcode/builds/.*jcode-' >/dev/null 2>&1; }
kill_jcode && sleep 1

tmux kill-session -t "$SESSION" 2>/dev/null

WORKDIR="$(mktemp -d /tmp/agent-demo-XXXX)"
cd "$WORKDIR"

export PATH="$NODE_BIN:$HOME/.local/bin:$PATH"
# jcode will not start its server without a credential; clank and pi both reach
# a prompt without one. Nothing is ever sent to it.
export ANTHROPIC_API_KEY="${ANTHROPIC_API_KEY:-sk-ant-demo-placeholder-not-a-real-key}"
export JCODE_NO_TELEMETRY=1

# -------------------------------------------------------------------- panes --
hold() { echo "$1; printf '\n[%s exited - press a key]'; read -rsn1" ; }

tmux new-session -d -s "$SESSION" -c "$WORKDIR" \
  -x "${COLUMNS:-$(tput cols 2>/dev/null || echo 200)}" \
  -y "${LINES:-$(tput lines 2>/dev/null || echo 50)}" \
  "$(hold "LD_LIBRARY_PATH='$SSL_LIB' '$CLANK_BIN'" clank)"

tmux split-window -h -t "$SESSION" -c "$WORKDIR" "$(hold "'$PI_BIN'" pi)"

tmux split-window -h -t "$SESSION" -c "$WORKDIR" \
  "$(hold "'$JCODE_BIN' --no-update --no-selfdev" jcode)"

tmux select-layout -t "$SESSION" even-horizontal
# Anchor the split to the window rather than "the active pane", which may have
# already exited (jcode gives up quickly if its server never came up) and taken
# the layout with it.
tmux split-window -v -f -l 12 -t "${SESSION}:0.0" -c "$WORKDIR" "'$SELF' --monitor"
MONITOR_PANE="$(tmux list-panes -t "$SESSION" -F '#{pane_index}' | tail -1)"

tmux set-option -t "$SESSION" -g pane-border-status top
tmux set-option -t "$SESSION" -g mouse on
tmux bind-key '&' kill-session

trap 'tmux kill-session -t "$SESSION" 2>/dev/null; kill_jcode; rm -rf "$WORKDIR"' EXIT

tmux attach -t "$SESSION"
