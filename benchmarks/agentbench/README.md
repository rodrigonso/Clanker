# agentbench

Compares coding-agent CLIs on memory on **macOS**: **clanker**, **jcode**,
**pi**, **Claude Code** and **opencode**. It uses macOS process inspection and
POSIX PTYs. Windows users can run Clanker's native in-process benchmark,
`clanker_tests --benchmark`, instead.

```bash
# from the repository root
bash tools/build.sh --config Release
export CLANKER_BIN="$PWD/build/clanker"
cd benchmarks
python3 -m agentbench doctor
python3 -m agentbench measure --tools clanker --sessions 1 --repeat 5
python3 -m agentbench measure --sessions 1 --repeat 5
python3 -m agentbench demo
python3 -m agentbench watch
```

## Setup

Install each competitor's macOS CLI and put it on `PATH`. Alternatively, set
`CLANKER_BIN`, `JCODE_BIN`, `PI_BIN`, `CLAUDE_BIN` or `OPENCODE_BIN` to its
executable path. Clanker falls back to this checkout's `build/clanker`.
`NODE_BIN`, when set, is a directory prepended to `PATH` for Node-based tools.
Clanker needs no benchmark-specific OpenSSL loader override.

`demo` additionally needs tmux (`brew install tmux`). `doctor` checks tool
paths, process inspection and memory, and warns about existing jcode daemons.
Run competitors once interactively before measuring so their required setup
and model downloads have finished.

## What it measures

Each tool is launched as its real interactive TUI on a 24x80 PTY. The harness
answers terminal capability queries, waits for an echoed probe, settles, then
sums resident set size (RSS) over its descendants and process groups. The probe
is typed but **never submitted**: this measures idle-at-input-ready footprint,
not a model turn. The probe is repeated because some TUIs discard input while
starting. The PTY's window size is also set with `TIOCSWINSZ`, not just reported
in terminal replies.

The macOS implementation samples `ps` for process ancestry, process groups and
RSS; `libproc` resolves executable images for live matching. Numbers are in
MiB (1024 x 1024 bytes). JSON output records `platform`, `metric`, `unit` and
`rss_mb`, so it cannot be mistaken for the old `pss_mb` output.

**RSS counts shared pages in every process.** Totals across sessions can
double-count shared libraries and must not be compared directly with PSS or
with another operating system's memory metric. No macOS comparison results
have been established yet.

jcode serves multiple clients from a detached daemon. `measure` starts its
own daemon on a private socket, includes that daemon's tree in the total, and
tears it down after each repeat. Its two configurations are measured separately,
not inferred from another process's environment.

`watch` and `demo` match executable images and use one jcode entry. They cannot
distinguish configuration variants or attribute arbitrary helper executables.
`jcode-embed-off` is therefore a `measure`-only choice. Use `measure`, not a live
view, for whole-tree numbers.

## Fairness and limitations

The harness originated from jcode's
[`scripts/bench_memory_cli.py`](https://github.com/1jehuang/jcode/blob/master/scripts/bench_memory_cli.py)
(MIT). The macOS implementation uses RSS rather than that script's metric.

- Every tool gets a throwaway `HOME` during `measure`.
- Claude Code's onboarding and folder-trust prompts are pre-answered so the
  measurement reaches an agent prompt, not a setup wizard.
- pi's populated tool cache is shared with the real profile to measure a warm
  installation rather than first-run downloads.
- jcode uses an isolated daemon, disables telemetry and self-updates, and takes
  its existing configuration and embedding model warm from the real profile.
  The embedding-off variant does not load that model.
- A daemon that cannot become ready produces no measurement. The `ready`
  column reports probe echoes; incomplete readiness means startup may still be
  in progress and the result is not an idle footprint.
- `demo` uses the real profile. Its jcode daemon can remain until the tool's
  idle timeout after the demo ends; `measure` owns and tears down its daemon.
- A detached demo keeps its working directory for its running panes.
- Concurrent sessions of large tools can exhaust memory. Start with one tool
  and one session before increasing concurrency.

## Historical measurements

The retired PSS-based implementation recorded these medians on Ubuntu 24.04
(WSL2), 4 CPUs and 7.7 GB RAM. They are provenance, **not macOS or Windows
results**, and there is no longer a supported runner for reproducing them.

| tool | 1 session | 10 sessions | per added session |
|---|---:|---:|---:|
| clanker | 4.9 MB | 13.7 MB | ~1.0 MB |
| jcode (local embedding off) | 98.4 MB | 156.0 MB | ~6.4 MB |
| jcode | 202.1 MB | 259.5 MB | ~6.4 MB |
| pi | 161.0 MB | 890.2 MB | ~81.0 MB |
| Claude Code | 296.4 MB | 1701.7 MB | ~156.1 MB |
| opencode | 867.1 MB | 5663.0 MB | ~532.9 MB |

Versions: Clanker `5df61d0` (Release), jcode `v0.71.1`, pi `0.74.2`,
Claude Code `2.1.226`, opencode `1.18.15`. Ten sessions means ten simultaneous
clients, not ten turns. The staged OpenSSL copy was charged in full by the
historical accounting. These values cannot establish current-platform ratios.

## Layout and offline checks

| file | role |
|---|---|
| `tools.py` | binary registry, process matching and first-run setup |
| `proc.py` | macOS RSS, process ancestry and executable inspection |
| `session.py` | POSIX PTY launch, terminal replies and readiness |
| `measure.py` / `watch.py` / `demo.py` / `doctor.py` | CLI subcommands |

From `benchmarks`, run `python3 -m unittest agentbench.test_proc` for mocked
offline process-accounting and CLI checks. These checks also run with `python`
on Windows; actually launching and measuring the TUIs requires macOS.
