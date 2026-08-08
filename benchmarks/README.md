# Benchmarks

Two suites, measuring different things.

| suite | measures | run it |
|---|---|---|
| [`agentbench/`](agentbench/) | **memory** — idle footprint of the real TUI, Clank against pi, Claude Code and opencode | `python3 -m agentbench measure` |
| [`pi-agent-bench/`](pi-agent-bench/) | **latency** — agent-loop turnaround, Clank against pi's `AgentHarness` | `node run-bench.mjs` |

`agentbench` runs the actual interactive programs and measures what a session
costs to have open. `pi-agent-bench` drives each harness's agent loop against a
faux provider and measures what a turn costs to run. Neither substitutes for
the other.

Both are Linux-friendly; `agentbench` is Linux-only, since PSS has no Windows
equivalent. On Windows, run it under WSL.

For Clank's own in-process numbers — agent-loop latency and peak RSS on a
scripted workload — see `clank_tests --benchmark` from the repository root.
