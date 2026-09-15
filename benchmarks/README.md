# Benchmarks

Clanker's native in-process benchmark works on **macOS and Windows**. After
building the tests through the repository scripts, run `clanker_tests
--benchmark` to measure agent-loop latency and peak RSS on a scripted workload,
with no second runtime involved.

[`agentbench/`](agentbench/) compares coding-agent CLIs on **memory** on macOS:
Clanker against jcode, pi, Claude Code and opencode. It launches each tool's
interactive TUI and measures process-tree **RSS** at an idle prompt.

```bash
cd benchmarks
python3 -m agentbench doctor      # check the macOS setup
python3 -m agentbench measure     # the numbers
python3 -m agentbench demo        # side by side in tmux
```

The interactive comparison needs macOS PTYs and process inspection; use the
native in-process benchmark on Windows. RSS totals count shared pages in each
process and are not interchangeable with the historical PSS measurements.

[`qualitybench/`](qualitybench/) retains external DeepSWE reference results,
an oracle-only task verifier, and a local auditor for historical job artifacts.
Its Clanker container adapter and binary bootstrap have been removed: Clanker
supports macOS and Windows, not the task containers' operating system.

```powershell
Set-Location benchmarks\qualitybench
python test_benchmark_flow.py
python audit_job.py <existing-job-directory>
python run_benchmark.py oracle --dry-run
```

On macOS, use `cd benchmarks/qualitybench` and `python3` for the same commands.
The oracle runs only external reference solutions, not Clanker; a real oracle
run requires Pier and its chosen environment backend.
