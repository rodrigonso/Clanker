# Benchmarks

[`agentbench/`](agentbench/) — compares coding-agent CLIs on **memory**: Clanker
against jcode, pi, Claude Code and opencode. It launches each tool's real
interactive TUI and measures process-tree PSS at an idle prompt.

```bash
cd benchmarks
python3 -m agentbench doctor      # check the setup
python3 -m agentbench measure     # the numbers
python3 -m agentbench demo        # side by side in tmux
```

Linux only (PSS has no Windows analogue); on Windows, run it under WSL.

[`qualitybench/`](qualitybench/) — compares them on **task success** instead, by
exposing Clanker as an agent to the Pier and Harbor harnesses so it can be run
against DeepSWE v1.1 with the model held fixed.

```bash
cd benchmarks/qualitybench
python3 test_clanker_agent.py       # adapter checks, no harness needed
python3 test_benchmark_flow.py    # safe runner and job-audit checks
python3 run_benchmark.py canary   # 10-task integration gate
```

For Clanker's own in-process numbers — agent-loop latency and peak RSS on a
scripted workload, with no second runtime involved — use `clanker_tests
--benchmark` from the repository root.
