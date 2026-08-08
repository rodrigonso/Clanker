# Interactive CLI memory benchmark

Process-tree PSS for interactive coding-agent CLIs, measured the way
[jcode](https://github.com/1jehuang/jcode) measures it, with Clank added.

`bench_memory_cli.py` is a port of jcode's own
[`scripts/bench_memory_cli.py`](https://github.com/1jehuang/jcode/blob/master/scripts/bench_memory_cli.py)
(MIT). The measurement is deliberately unchanged so the numbers line up with
jcode's published table; the differences are listed under
[Deviations](#deviations-from-the-upstream-script) below.

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

`demo.sh` launches all three agents in tmux panes with a live PSS readout along
the bottom:

```bash
./demo.sh              # three agents + monitor
./demo.sh --monitor    # just the monitor, against whatever is already running
```

`Ctrl-b d` detaches, `Ctrl-b &` kills it. Binaries come from the same
`CLANK_BIN` / `PI_BIN` / `JCODE_BIN` variables the benchmark uses.

Two things the script handles that are easy to get wrong by hand: jcode's
launcher is a shim that execs a versioned binary out of `~/.jcode/builds`, and
the daemon it spawns is that same binary rather than a child of the client — so
matching on the launcher path finds neither. It also kills a leftover daemon
before starting, because otherwise the next run reuses a server nobody paid
for and jcode looks cheaper than it is.

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
python3 bench_memory_cli.py --sessions 10 --repeat 2 --tools jcode_memory_off
```

Binaries are located via `CLANK_BIN`, `PI_BIN`, and `JCODE_BIN`, defaulting to
`~/clank-src/build-linux/clank`, `~/.local/npm/bin/pi`, and
`~/.local/bin/jcode`. `--repeat` reports the median, because a tool sampled
while still initialising produces a high outlier rather than symmetric noise.

## Results

Ubuntu 24.04 (WSL2), 4 CPU, 7.7 GB RAM. Clank at `243fab8`, Release.
Medians across repeats.

| tool | 1 session | 10 sessions | per added session |
|---|---:|---:|---:|
| **clank** | **3.3 MB** | **13.0 MB** | **~1.1 MB** |
| jcode (embedding off) | 96.0 MB | 155.3 MB | ~6.6 MB |
| jcode (default) | 97.3 MB | 291.1 MB | ~21.5 MB |
| pi | 157.6 MB | 1049.4 MB | ~99.1 MB |

Versions: clank `243fab8`, pi `0.74.2`, jcode `v0.71.1 (dd8755f7e)`.

At one session Clank is **29× lighter than jcode's best configuration** and
**48× lighter than pi**. At ten it is **12×** and **81×**. Clank is a single
process; jcode runs a client plus a shared server (11 processes for 10
sessions), which is what makes its per-session slope so much flatter than pi's.

These reproduce jcode's published ordering. Their table reports pi at 144.4 MB
for one session against 157.6 MB here, and 833.0 MB at ten against 1049.4 MB —
close enough, on a different machine and a different pi version, to trust the
port. Their own numbers for jcode are lower than these (27.8 MB / 117.0 MB),
which is discussed below.

## Deviations from the upstream script

Three, each of which makes the comparison *less* favourable to Clank, not more:

1. **Every tool gets a throwaway `HOME`.** Upstream isolates only jcode, into a
   temporary `JCODE_HOME` with symlinked credentials, and lets every other tool
   inherit the real home directory with whatever sessions and configuration are
   in it. Isolating all three removes that asymmetry.

2. **jcode's server is started explicitly, per run, on a private socket.** It
   outlives its client, so without this the first run pays for the daemon and
   later runs silently do not. Upstream does the same thing; it is called out
   here because it is easy to get wrong and it materially changes the number.

3. **jcode gets a placeholder `ANTHROPIC_API_KEY`.** Its server refuses to
   start without a credential. Clank and pi both reach an input prompt with no
   credential at all. Nothing is ever sent, so the placeholder only gets jcode
   to the same starting line the other two reach unaided.

The `jcode_memory_on` settle timings (5 s after the socket opens, 2 s per
session) are copied from upstream, which gives the embedding build extra time
to finish initialising before sampling.

## Why these jcode numbers are higher than jcode's own

Their table reports 27.8 MB for one session; this measures 96.0 MB for the same
configuration. Not reconciled, and worth stating plainly rather than assuming
the difference favours us. Candidates, in rough order of likelihood: a
different jcode version (theirs `v0.9.1888-dev`, this `v0.71.1`); WSL2 rather
than bare-metal Linux, which changes page sharing; and their run reusing a warm
shared server across tools while this starts a cold one per run. The relative
ordering is unaffected — Clank is far below every configuration either way —
but the absolute jcode figures here should not be quoted as refuting theirs.

## Caveats

- Not all sessions always reach input-ready within the timeout at 10 sessions
  (jcode most often). The summary line flags this as `input-ready N/M`. PSS is
  still summed over every process in the tree, and the totals were stable
  across a longer settle (`--settle 4 --timeout 40` gave 150.8 MB against
  155.3 MB), so the figure is not an artefact of sampling early.
- One machine, one OS, one run of each. Directional, not a published claim.
- Idle footprint only. What an agent costs *while working* is a different
  measurement; for Clank that is `clank_tests --benchmark`, which reports peak
  RSS alongside latency.
