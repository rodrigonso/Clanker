# Benchmarks

[`agentbench/`](agentbench/) — compares coding-agent CLIs on **memory**: Clank
against pi, Claude Code and opencode. It launches each tool's real interactive
TUI and measures process-tree PSS at an idle prompt.

```bash
cd benchmarks
python3 -m agentbench doctor      # check the setup
python3 -m agentbench measure     # the numbers
python3 -m agentbench demo        # side by side in tmux
```

Linux only (PSS has no Windows analogue); on Windows, run it under WSL.

For Clank's own in-process numbers — agent-loop latency and peak RSS on a
scripted workload, with no second runtime involved — use `clank_tests
--benchmark` from the repository root.
