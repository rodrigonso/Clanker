# agentbench

Compares coding-agent CLIs on memory: **clank**, **jcode**, **pi**,
**Claude Code** and **opencode**. Linux only (PSS has no Windows analogue) — on
Windows, run it under WSL.

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

Each tool is launched as its real interactive TUI on a pty sized 24×80. The
harness answers the terminal capability queries a TUI probes for (cursor
position, device attributes, OSC colours, XTGETTINCAP, kitty graphics) so the
app renders as it would for a user, waits until it echoes a typed probe,
settles, then sums `Pss` from `/proc/<pid>/smaps_rollup` across the whole
process tree *and* process group — so a tool that spawns helpers or a daemon is
charged for them.

The window size is set on the pty with `TIOCSWINSZ` as well as reported in the
reply to `CSI 18 t`, because TUIs disagree about which to trust. Most ask; an
ioctl-based one reads a bare pty as 0×0 and draws nothing at all, which is what
jcode did before the size was set.

**Not every tool's processes are its descendants.** jcode serves all its
sessions from one daemon that it `setsid()`s away from the client, putting it in
neither the client's descendant tree nor its process group. Walking from the
client alone would miss roughly two thirds of jcode's footprint — an error in
Clank's favour, which is exactly what the fairness rules below exist to prevent.
So a tool may declare a `setup` hook that starts its daemon on an isolated
socket, hands back the daemon's pid to be charged for, and has it torn down with
the run. That also stops a daemon surviving on its idle timeout into the next
repeat, and stops the benchmark colliding with a daemon the user is really
using.

**No prompt is ever submitted.** The probe is typed but never sent, so this is
idle-at-input-ready footprint, not the cost of running a turn.

Readiness is echo, not paint. A TUI can draw a full screen well before it is
listening — opencode discards anything typed during startup — so the probe is
re-sent every two seconds. Probing once reported opencode as never ready and
sampled it ~10% low, mid-initialisation.

## Results

Ubuntu 24.04 (WSL2), 4 CPU, 7.7 GB RAM. Clank at `5df61d0`, Release.
Medians across repeats.

| tool | 1 session | 10 sessions | per added session |
|---|---:|---:|---:|
| **clank** | **4.9 MB** | **13.7 MB** | **~1.0 MB** |
| jcode (local embedding off) | 98.4 MB | 156.0 MB | ~6.4 MB |
| jcode | 202.1 MB | 259.5 MB | ~6.4 MB |
| pi | 161.0 MB | 890.2 MB | ~81.0 MB |
| Claude Code | 296.4 MB | 1701.7 MB | ~156.1 MB |
| opencode | 867.1 MB | 5663.0 MB | ~532.9 MB |

Versions: clank `5df61d0`, jcode `v0.71.1`, pi `0.74.2`, Claude Code `2.1.226`,
opencode `1.18.15`.

At one session Clank is **20× lighter than jcode with its local embedding
off** — its closest competitor — **33× lighter than pi**, **60× lighter than
Claude Code** and **177× lighter than opencode**. At ten it is **11×**, **65×**,
**124×** and **413×**. Ten opencode sessions need 5.7 GB; ten Clank sessions
need 14 MB.

"10 sessions" means ten instances running concurrently, not ten turns — it
isolates what each *additional* agent costs, which is the number that matters
when running several at once.

jcode is the only other tool with a shallow slope, and it gets there a different
way: one daemon serves every session, so ten sessions are ten thin clients
against a single server rather than ten copies of the agent. That is worth about
6.4 MB per added session against Clank's 1.0 MB, but it also means jcode's first
session carries the whole server — 98 MB before the embedding model, 202 MB with
it. Clank has no daemon; its floor and its slope are the same 1–5 MB process.

Clank's 4.9 MB is 1.8 MB of Clank plus 3.1 MB of OpenSSL, which this build
links from a privately staged copy that nothing else on the machine maps, so
PSS charges it in full. A distro-packaged build sharing the system libssl
measures nearer 3.3 MB. The higher figure is reported because it is what the
documented build produces. It is also why the per-session slope sits below the
single-session cost: the ten processes share one copy of OpenSSL.

## Fairness

The measurement started as a port of jcode's
[`scripts/bench_memory_cli.py`](https://github.com/1jehuang/jcode/blob/master/scripts/bench_memory_cli.py)
(MIT). jcode is now measured here too rather than only cross-referenced, so the
deviations below are listed in both directions: the ones that cost Clank and
the ones that flatter a competitor.

Deviations that make the comparison *less* favourable to Clank:

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
- **The pty is given a real window size.** Without it jcode's TUI never paints
  and would be sampled before it finishes attaching.

Deviations that favour **jcode**, all of them inherited from its own script so
that the numbers stay comparable to the table it publishes:

- **Its daemon is started explicitly** with `jcode serve` on an isolated socket,
  rather than letting the first client spawn one. One daemon serves all ten
  sessions in the 10-session case, which is the architecture jcode is designed
  around and the reason its per-session slope is low.
- **`JCODE_NO_TELEMETRY=1`**, so no telemetry work is charged to it.
- **Credentials and the embedding model are taken warm** from the real `~/.jcode`
  when present — auth files copied (jcode rejects symlinked ones), the 87 MB
  `models` directory symlinked for the default variant so it is measured loaded
  rather than mid-download.
- **A placeholder `ANTHROPIC_API_KEY` is set** if no real credential is found,
  because jcode's server exits rather than start without one. Nothing is ever
  sent — the probe is typed and never submitted — and jcode's own idle-profiling
  scripts do the same.
- **`--no-update`** stops jcode self-updating mid-benchmark and changing the
  version under measurement.

Two honest notes on the jcode figures specifically. In a throwaway `JCODE_HOME`
its client shows a first-run notice instead of the usual screen; seeding the
profile's first-run markers so it opens normally moves idle PSS by under 2 MB,
inside run-to-run spread, so the notice is left alone and the home kept clean.
And jcode publishes 27.8 MB / 167.1 MB for the two configurations against the
98.4 MB / 202.1 MB measured here on v0.71.1. That gap is confined to the first
session: raising the settle to six seconds does not move it, and the daemon plus
client account for the total between them, so it is a difference in the baseline
each side starts from rather than in the slope.

The slope is the part that can be checked against jcode directly, because the
figure [jcode.sh](https://jcode.sh) headlines is marginal PSS per *added*
session rather than an absolute total — the same column as this table's last
one:

| tool | jcode publishes | measured here |
|---|---:|---:|
| jcode | ~10.4 MB | ~6.4 MB |
| jcode (local embedding off) | ~9.9 MB | ~6.4 MB |
| pi | ~76.5 MB | ~81.0 MB |
| Claude Code | ~212.7 MB | ~156.1 MB |

pi lands within 6% of the published number and the ordering matches throughout,
which is the agreement worth having: two independent harnesses on different
machines and different versions costing the same architectures the same way.
Compare the absolute columns only with that caveat attached.

For the rest, upstream's absolute table reports pi at 144.4 MB for one session
against 161.0 MB here, Claude Code at 386.6 MB against 296.4 MB, and opencode at
371.5 MB against 867.1 MB — the same ordering on a different machine and
different versions, though opencode diverges most, so treat its absolute figure
with the same caution.

## Layout

| file | role |
|---|---|
| `tools.py` | tool registry — binaries, env, process matching, first-run setup |
| `proc.py` | PSS accounting, process-tree walk, ownership |
| `session.py` | pty launch, terminal query replies, readiness probe |
| `measure.py` / `watch.py` / `demo.py` / `doctor.py` | the four subcommands |

Adding a tool is one entry in `tools.py`; every subcommand reads from it.

Three things the registry exists to get right. Claude Code and opencode ship
native ELF binaries — named `.exe` on every platform despite installing from
npm — so they are matched by resolved path, while pi is a node script whose
argv is rewritten and has to be matched through node. Each tool carries its own
environment: clank needs `LD_LIBRARY_PATH` for its staged OpenSSL, pi needs
`PATH` to reach node or its shebang fails silently. And an entry may declare an
`image` hook for a binary that is really a launcher — `jcode` on disk is a
505-byte `sh` script that `exec`s a sibling 156 MB ELF, so without following
that hop it would be reported as 0 MB and match no process at all.

The two jcode entries share one binary, so path matching alone would attribute
every jcode process to whichever was registered first. They are told apart by
reading `JCODE_MEMORY_ENABLED` out of `/proc/<pid>/environ`, which is why
`matches` is handed a pid.

## Setup

```bash
# clank, built for Linux (stock flags; libcurl is vendored, OpenSSL is not)
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux --target clank

# the others
npm install -g @earendil-works/pi-coding-agent @anthropic-ai/claude-code opencode-ai
curl -fsSL https://jcode.sh/install | bash

sudo apt install -y tmux   # demo only
```

Binaries are found via `CLANK_BIN`, `JCODE_BIN`, `PI_BIN`, `CLAUDE_BIN`,
`OPENCODE_BIN`, defaulting under `~`. `agentbench doctor` reports what it found
and what is missing, and warns if a jcode server is already running — a stale
shared daemon is the likeliest source of a bad jcode number.

## Caveats

- Ten concurrent opencode sessions need ~6 GB. Run that case on its own.
- jcode's first run downloads an embedding model into `~/.jcode/models`. Until
  it is there the default variant is measured mid-download; run jcode once by
  hand before quoting it.
- `demo` and `watch` list one jcode entry, not two. Both attach to the real
  `HOME`, where a single daemon serves every pane, so a second jcode pane would
  be charged against whichever entry matched first. A demo pane also leaves that
  daemon running for its five-minute idle timeout after the demo exits; `measure`
  does not, because it runs its own on a private socket and kills it.
- The demo uses your real `HOME`, so each tool uses whatever credential it has.
  It deliberately sets no placeholder API key — a fake one is picked up by every
  tool and makes each request fail.
- The demo is for the visual; numbers move as each TUI settles. Quote `measure`.
- One machine, one OS. Directional, not a published claim.
- Idle footprint only. What an agent costs *while working* is a different
  measurement; for Clank that is `clank_tests --benchmark`, which reports
  agent-loop latency and peak RSS on a scripted workload.
