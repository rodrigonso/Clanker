# agentbench

Compares coding-agent CLIs on memory: **clank**, **pi**, **Claude Code** and
**opencode**. Linux only (PSS has no Windows analogue) — on Windows, run it
under WSL.

```bash
cd benchmarks
python3 -m agentbench doctor                          # check the setup
python3 -m agentbench measure --sessions 1 --repeat 5 # the numbers
python3 -m agentbench demo                            # side by side in tmux
python3 -m agentbench watch                           # live readout only
```

It runs straight from the repo, including over `/mnt/c` from WSL — pure
Python, so the CRLF line endings that stop a shell script are not a problem.
Nothing needs copying into the Linux filesystem first.

## What it measures

Each tool is launched as its real interactive TUI on a pty. The harness answers
the terminal capability queries a TUI probes for (cursor position, device
attributes, OSC colours, XTGETTINCAP, kitty graphics) so the app renders as it
would for a user, waits until it echoes a typed probe, settles, then sums `Pss`
from `/proc/<pid>/smaps_rollup` across the whole process tree *and* process
group — so a tool that spawns helpers or a daemon is charged for them.

**No prompt is ever submitted.** The probe is typed but never sent, so this is
idle-at-input-ready footprint, not the cost of running a turn.

Readiness is echo, not paint. A TUI can draw a full screen well before it is
listening — opencode discards anything typed during startup — so the probe is
re-sent every two seconds. Probing once reported opencode as never ready and
sampled it ~10% low, mid-initialisation.

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

"10 sessions" means ten instances running concurrently, not ten turns — it
isolates what each *additional* agent costs, which is the number that matters
when running several at once.

Clank's 4.9 MB is 1.8 MB of Clank plus 3.1 MB of OpenSSL, which this build
links from a privately staged copy that nothing else on the machine maps, so
PSS charges it in full. A distro-packaged build sharing the system libssl
measures nearer 3.3 MB. The higher figure is reported because it is what the
documented build produces. It is also why the per-session slope sits below the
single-session cost: the ten processes share one copy of OpenSSL.

## Fairness

The measurement started as a port of jcode's
[`scripts/bench_memory_cli.py`](https://github.com/1jehuang/jcode/blob/master/scripts/bench_memory_cli.py)
(MIT), so results line up with the RAM table jcode publishes. It reports pi at
144.4 MB for one session against 162.7 MB here, Claude Code at 386.6 MB against
297.1 MB, and opencode at 371.5 MB against 799.7 MB — the same ordering on a
different machine and different versions, though opencode diverges most, so
treat its absolute figure as the least settled of the four.

Deviations from that script, each of which makes the comparison *less*
favourable to Clank:

- **Every tool gets a throwaway `HOME`.** Upstream isolates only jcode and lets
  the others inherit the real profile with whatever sessions are in it.
- **Claude Code's onboarding is pre-answered.** In a fresh `HOME` it opens on a
  theme picker and then a folder-trust prompt; sampling there measures a wizard,
  not an agent — 235 MB against 295 MB once it reaches a real prompt.
- **pi's tool cache is shared with the real profile.** It downloads `fd` and
  `ripgrep` on first run, so the throwaway home symlinks the populated cache and
  pi is measured warm like the others.
- **The probe is re-sent rather than typed once**, and more terminal queries are
  answered, so a slow-starting TUI is measured at a prompt rather than mid-boot.

## Layout

| file | role |
|---|---|
| `tools.py` | tool registry — binaries, env, process matching, first-run setup |
| `proc.py` | PSS accounting, process-tree walk, ownership |
| `session.py` | pty launch, terminal query replies, readiness probe |
| `measure.py` / `watch.py` / `demo.py` / `doctor.py` | the four subcommands |

Adding a tool is one entry in `tools.py`; every subcommand reads from it.

Two things the registry exists to get right. Claude Code and opencode ship
native ELF binaries — named `.exe` on every platform despite installing from
npm — so they are matched by resolved path, while pi is a node script whose
argv is rewritten and has to be matched through node. And each tool carries its
own environment: clank needs `LD_LIBRARY_PATH` for its staged OpenSSL, pi needs
`PATH` to reach node or its shebang fails silently.

## Setup

```bash
# clank, built for Linux (stock flags; libcurl is vendored, OpenSSL is not)
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux --target clank

# the others
npm install -g @earendil-works/pi-coding-agent @anthropic-ai/claude-code opencode-ai

sudo apt install -y tmux   # demo only
```

Binaries are found via `CLANK_BIN`, `PI_BIN`, `CLAUDE_BIN`, `OPENCODE_BIN`,
defaulting under `~`. `agentbench doctor` reports what it found and what is
missing.

## Caveats

- Ten concurrent opencode sessions need ~6 GB. Run that case on its own.
- The demo uses your real `HOME`, so each tool uses whatever credential it has.
  It deliberately sets no placeholder API key — a fake one is picked up by every
  tool and makes each request fail.
- The demo is for the visual; numbers move as each TUI settles. Quote `measure`.
- One machine, one OS. Directional, not a published claim.
- Idle footprint only. What an agent costs *while working* is a different
  measurement — for Clank that is `clank_tests --benchmark`, and
  `../pi-agent-bench` compares agent-loop latency against pi's harness.
