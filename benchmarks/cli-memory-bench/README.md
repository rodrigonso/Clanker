# Interactive CLI memory benchmark

Process-tree PSS for interactive coding-agent CLIs: Clank against pi,
Claude Code and opencode.

The measurement follows [jcode](https://github.com/1jehuang/jcode)'s
[`scripts/bench_memory_cli.py`](https://github.com/1jehuang/jcode/blob/master/scripts/bench_memory_cli.py)
(MIT), which `bench_memory_cli.py` began as a port of, so the numbers are
comparable with the RAM table jcode publishes. Differences are listed under
[Deviations](#deviations-from-the-upstream-script).

## What it measures

Each tool is launched as its real interactive TUI on a pty. The script answers
the terminal capability queries a TUI probes for (cursor position, device
attributes, OSC 10/11 colours, and so on) so the app renders as it would for a
user. It waits for the first meaningful line of screen content, types a short
probe string, waits for that probe to echo back, settles, and then sums `Pss`
from `/proc/<pid>/smaps_rollup` across the whole process tree *and* process
group — so a tool that spawns helpers or a background daemon is charged for
them.

**No prompt is ever submitted.** The probe is typed but never sent, so this is
idle-at-input-ready footprint, not the cost of running a turn. That is also
true of jcode's published RAM table, which this reproduces.

PSS is Linux-only. On Windows, run this under WSL.

## Side-by-side demo

`demo.sh` launches agents in tmux panes with a live PSS readout along the
bottom:

```bash
./demo.sh                       # clank, pi, claude and opencode
./demo.sh clank claude          # pick your own, any two to four
./demo.sh --monitor             # just the monitor, against what is running
```

Known tools: `clank`, `pi`, `claude`, `opencode`. `Ctrl-b d` detaches,
`Ctrl-b &` kills it. Binaries come from `CLANK_BIN` / `PI_BIN` / `CLAUDE_BIN` /
`OPENCODE_BIN`.

The demo uses your real `HOME`, so each tool uses whatever credential it
already has. It deliberately does *not* set a placeholder API key — a fake key
is picked up by every tool and makes each request fail.

One detail the monitor has to get right: Claude Code and opencode both ship
native ELF binaries (named `.exe` on every platform, despite installing from
npm), so they are matched by resolved path like Clank, while pi is a node
script whose argv is rewritten and has to be matched through node.

This is a demo, not the measurement. Numbers on screen move around as each TUI
settles; use `bench_memory_cli.py` for figures worth quoting.

## Running the benchmark

Build Clank for Linux first (it builds with stock flags; only OpenSSL headers
are needed, and libcurl is vendored):

```bash
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux --target clank
```

Then:

```bash
python3 bench_memory_cli.py --sessions 1  --repeat 5
python3 bench_memory_cli.py --sessions 10 --repeat 2 --tools claude
```

Binaries are located via `CLANK_BIN`, `PI_BIN`, `CLAUDE_BIN` and
`OPENCODE_BIN`, defaulting to `~/clank-src/build-linux/clank`,
`~/.local/npm/bin/pi`, `~/.local/npm/bin/claude` and
`~/.local/npm/bin/opencode`. `--repeat` reports the median, because a tool
sampled while still initialising produces a high outlier rather than symmetric
noise.

## Results

Ubuntu 24.04 (WSL2), 4 CPU, 7.7 GB RAM. Clank at `243fab8`, Release.
Medians across repeats.

| tool | 1 session | 10 sessions | per added session |
|---|---:|---:|---:|
| **clank** | **4.9 MB** | **13.4 MB** | **~0.9 MB** |
| pi | 162.7 MB | 911.9 MB | ~83.2 MB |
| Claude Code | 297.1 MB | 1710.6 MB | ~157.1 MB |
| opencode | 799.7 MB | 5576.5 MB | ~530.8 MB |

Versions: clank `243fab8`, pi `0.74.2`, Claude Code `2.1.226`,
opencode `1.18.15`.

At one session Clank is **33× lighter than pi**, **61× lighter than Claude
Code** and **163× lighter than opencode**. At ten it is **68×**, **128×** and
**416×**. Ten opencode sessions need 5.6 GB; ten Clank sessions need 13 MB.

Clank's 4.9 MB is 1.8 MB of Clank plus 3.1 MB of OpenSSL, which this build
links from a privately staged copy that nothing else on the machine maps, so
PSS charges it in full. A distro-packaged build sharing the system libssl
measures nearer 3.3 MB. The higher figure is the one reported here because it
is what the documented build produces. It also explains why the per-session
slope is below the single-session cost: the ten processes share one copy of
OpenSSL between them.

These reproduce the ordering in jcode's published table. It reports pi at
144.4 MB for one session against 162.7 MB here, Claude Code at 386.6 MB against
297.1 MB, and opencode at 371.5 MB against 799.7 MB — the same ordering on a
different machine and different versions, though opencode is the one that
diverges most, so treat its absolute figure as the least settled of the four.

## Deviations from the upstream script

Four, each of which makes the comparison *less* favourable to Clank, not more:

1. **Every tool gets a throwaway `HOME`.** Upstream isolates only jcode, into a
   temporary `JCODE_HOME` with symlinked credentials, and lets every other tool
   inherit the real home directory with whatever sessions and configuration are
   in it. Isolating all of them removes that asymmetry.

2. **Claude Code's onboarding is pre-answered.** In a fresh `HOME` it opens on
   a theme picker and then a folder-trust prompt, and sampling there measures a
   wizard rather than an agent session — 235 MB against 295 MB once it reaches
   a real prompt. The script writes a `.claude.json` marking both as answered.

3. **pi's tool cache is shared with the real profile.** It downloads `fd` and
   `ripgrep` into `~/.pi` on first run; the throwaway home symlinks the
   populated cache so pi is measured warm, as the others are.

4. **The readiness probe is re-sent every two seconds** rather than typed once
   when the first line of content appears. A TUI can paint a screen well before
   it is listening, and opencode drops anything typed during startup: probing
   once reported it as never input-ready and sampled it ~10% low, while it was
   still initialising. Upstream also answers fewer terminal queries than
   opencode asks — it probes with DCS and APC sequences (XTGETTINCAP, kitty
   graphics, iTerm2 capabilities) and waits on some, so those are answered too.

## Caveats

- Not all sessions always reach input-ready within the timeout at 10 sessions.
  The summary line flags this as `input-ready N/M`. PSS is still summed over
  every process in the tree, and totals were stable across a longer settle, so
  the figure is not an artefact of sampling early.
- One machine, one OS. Directional, not a published claim.
- Idle footprint only. What an agent costs *while working* is a different
  measurement; for Clank that is `clank_tests --benchmark`, which reports peak
  RSS alongside latency.
