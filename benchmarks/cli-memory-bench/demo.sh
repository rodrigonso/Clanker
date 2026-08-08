#!/usr/bin/env bash
# Launch coding agents side by side with a live memory readout.
#
#   ./demo.sh                     clank, pi and claude (the default three)
#   ./demo.sh clank pi jcode      pick your own, any two to four
#   ./demo.sh --monitor [tools]   just the monitor, against what is running
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
CLAUDE_BIN="${CLAUDE_BIN:-$HOME/.local/npm/bin/claude}"
SSL_LIB="${SSL_LIB:-$HOME/.local/ssl/lib}"
NODE_BIN="${NODE_BIN:-$HOME/.local/node/bin}"

DEFAULT_TOOLS=(clank pi claude)

bin_for() {
  case "$1" in
    clank)  echo "$CLANK_BIN" ;;
    pi)     echo "$PI_BIN" ;;
    jcode)  echo "$JCODE_BIN" ;;
    claude) echo "$CLAUDE_BIN" ;;
    *)      echo "" ;;
  esac
}

cmd_for() {
  case "$1" in
    clank)  echo "LD_LIBRARY_PATH='$SSL_LIB' '$CLANK_BIN'" ;;
    pi)     echo "'$PI_BIN'" ;;
    jcode)  echo "'$JCODE_BIN' --no-update --no-selfdev" ;;
    claude) echo "'$CLAUDE_BIN'" ;;
  esac
}

# ----------------------------------------------------------------- monitor --
if [[ "${1:-}" == "--monitor" ]]; then
  shift
  MON_TOOLS=("$@")
  [[ ${#MON_TOOLS[@]} -eq 0 ]] && MON_TOOLS=("${DEFAULT_TOOLS[@]}")
  MON_ARGS=()
  for t in "${MON_TOOLS[@]}"; do MON_ARGS+=("$t=$(bin_for "$t")"); done
  exec python3 - "${MON_ARGS[@]}" <<'PY'
import os, sys, time

SPECS = [a.split("=", 1) for a in sys.argv[1:]]
TOOLS = [name for name, _ in SPECS]
BIN = {name: path for name, path in SPECS}
REAL = {name: os.path.realpath(path) for name, path in SPECS}
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
    if "clank" in TOOLS and exe and exe == REAL["clank"]:
        return "clank"
    # Claude Code ships a native ELF binary (named claude.exe on every
    # platform), so it matches by path like clank does rather than via node.
    if "claude" in TOOLS and exe and exe == REAL["claude"]:
        return "claude"
    # jcode's launcher is a shim that execs a versioned binary out of
    # ~/.jcode/builds, and the daemon it spawns is that same binary, so match
    # the install tree instead of the launcher path.
    if "jcode" in TOOLS and exe and (exe == REAL["jcode"] or "/.jcode/builds/" in exe
                                     or os.path.basename(exe).startswith("jcode-")):
        return "jcode"
    # pi is a node script: exe is node and argv is rewritten to "pi", so
    # neither signal alone is enough.
    if exe and os.path.basename(exe) in ("node", "nodejs"):
        first = os.path.basename(cmd.split()[0]) if cmd.split() else ""
        if "pi" in TOOLS and (first == "pi" or "pi-coding-agent" in cmd or BIN["pi"] in cmd):
            return "pi"
    return None

PALETTE = ["\033[92m", "\033[93m", "\033[96m", "\033[95m"]
COLOR = {t: PALETTE[i % len(PALETTE)] for i, t in enumerate(TOOLS)}
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

# --------------------------------------------------------------- tool list --
TOOLS=("$@")
[[ ${#TOOLS[@]} -eq 0 ]] && TOOLS=("${DEFAULT_TOOLS[@]}")

# ------------------------------------------------------------------- checks --
missing=0
for t in "${TOOLS[@]}"; do
  path="$(bin_for "$t")"
  if [[ -z "$path" ]]; then
    echo "!! unknown tool '$t' (known: clank pi jcode claude)" >&2
    missing=1
  elif [[ ! -x "$path" ]]; then
    echo "!! $t not found at $path" >&2
    missing=1
  fi
done
if [[ $missing -eq 1 ]]; then
  echo >&2
  echo "Override with CLANK_BIN / PI_BIN / JCODE_BIN / CLAUDE_BIN, or build clank:" >&2
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
if [[ " ${TOOLS[*]} " == *" jcode "* ]]; then kill_jcode && sleep 1; fi

tmux kill-session -t "$SESSION" 2>/dev/null

WORKDIR="$(mktemp -d /tmp/agent-demo-XXXX)"
cd "$WORKDIR"

export PATH="$NODE_BIN:$HOME/.local/npm/bin:$HOME/.local/bin:$PATH"
export JCODE_NO_TELEMETRY=1
# NOTE: no placeholder ANTHROPIC_API_KEY here. Whatever credential you already
# have -- an exported key, or each tool's own stored login -- is what gets used.
# A fake key set here would be picked up by every tool and 401 every request.

# -------------------------------------------------------------------- panes --
hold() { echo "$1; printf '\n[%s exited - press a key]'; read -rsn1" ; }

tmux new-session -d -s "$SESSION" -c "$WORKDIR" \
  -x "${COLUMNS:-$(tput cols 2>/dev/null || echo 200)}" \
  -y "${LINES:-$(tput lines 2>/dev/null || echo 50)}" \
  "$(hold "$(cmd_for "${TOOLS[0]}")" "${TOOLS[0]}")"

for t in "${TOOLS[@]:1}"; do
  tmux split-window -h -t "$SESSION" -c "$WORKDIR" "$(hold "$(cmd_for "$t")" "$t")"
done

tmux select-layout -t "$SESSION" even-horizontal
# Anchor the split to pane 0 rather than "the active pane", which may have
# already exited and taken the layout with it.
tmux split-window -v -f -l 12 -t "${SESSION}:0.0" -c "$WORKDIR" \
  "'$SELF' --monitor ${TOOLS[*]}"

tmux set-option -t "$SESSION" -g pane-border-status top
tmux set-option -t "$SESSION" -g mouse on
tmux bind-key '&' kill-session

trap 'tmux kill-session -t "$SESSION" 2>/dev/null; kill_jcode; rm -rf "$WORKDIR"' EXIT

tmux attach -t "$SESSION"
