# qualitybench

This directory retains **external benchmark references and historical job
auditing**, not a current Clanker task runner. Clanker supports macOS and
Windows only. The Pier/Harbor adapter and its Debian binary bootstrap were
removed because they required running a Clanker executable inside unsupported
task containers. There are no Clanker `canary` or `full` gates, binary-upload
options, or container installation hooks.

[`agentbench/`](../agentbench/) provides a native macOS interactive memory
comparison; `clanker_tests --benchmark` provides Clanker's in-process latency
and peak-RSS measurements on both macOS and Windows.

## Retained tools

- `audit_job.py` reads existing Pier job artifacts locally on macOS or Windows.
  It does not launch Clanker or a container. It rejects incomplete jobs,
  exceptions, destructive trial retries, missing patches or event logs, and
  truncated final responses. Empty patches are scored failures, not missing
  evidence. Token-based cost estimates use the historical prices in the file,
  not a live pricing feed.
- `run_benchmark.py oracle` runs **Pier's own oracle**, applying external
  reference patches and running task verifiers without Clanker or model calls.
  This is dataset validation, not a Clanker integration. Its `modal` and
  `docker` backends are external harness environments; neither receives a
  Clanker binary. Trial retries remain zero to preserve failure evidence.

From this directory on Windows:

```powershell
python test_benchmark_flow.py
python audit_job.py <existing-job-directory>
python run_benchmark.py oracle --dry-run
python run_benchmark.py oracle --tasks <deep-swe-task-directory> --concurrency 1
```

On macOS, use `python3` instead of `python`. Offline checks and auditing need
only Python. A real oracle run requires a separately installed
[Pier](https://github.com/datacurve-ai/pier), a
[DeepSWE](https://github.com/datacurve-ai/deep-swe) checkout, and the chosen
backend's own setup. Oracle runs make no model calls but may incur compute
costs. This repository does not bootstrap those environments.

DeepSWE tasks use 8192 MB of memory and 20480 MB of storage each; select
concurrency accordingly. `--environment docker` switches from the default
Modal backend. `--dry-run` prints the command without requiring installed
tasks or a harness.

### External verifier caveats

Pier's published `datacurve-pier` 0.3.0 predates task schema 1.3: it ignores
`[[verifier.collect]]`, producing an empty submission and reward zero even for
the oracle. Use a version that implements that schema. The recorded oracle
comparison was F2P 0/20 on 0.3.0 versus 20/20 on git HEAD for the same task.

The recorded Modal oracle sweep passed 112/113 tasks. Prometheus's ten IPv6
QueryLog cases skipped under gVisor with `ip version not supported`, and Pier
counted those skips as P2P failures. Validate that external task on an
appropriate backend before calling the dataset gate clean.

## Reading historical scores

`F2P` counts fail-to-pass tests; `P2P` is the pass-to-pass regression guard.
`reward` is all-or-nothing: 1.0 only when every F2P test passes and no P2P test
regresses. Published DeepSWE headline numbers are resolve rates (mean reward),
not F2P rates.

The collect hook grades committed work using `git diff <base_commit> HEAD`.
Uncommitted changes do not enter the submission. Audit artifacts before
quoting scores: even a job with zero final exceptions can have lost earlier
failed attempts to Pier's destructive retries.

## External reference numbers

Published by [jcode](https://jcode.sh/benchmarks/deepswe), DeepSWE v1.1,
113 tasks, dataset commit `8cae5984`. These are external reference results,
not measurements of the current Clanker platforms.

| harness (Claude Opus 4.8, high, k=1) | untimed | at the 5400 s cutoff | median |
|---|---:|---:|---:|
| OpenCode | 52.2% | 52.2% | 16.1 m |
| mini-SWE-agent | 51.3% | 51.3% | 12.1 m |
| jcode | 50.4% | 43.4% | 37.5 m |
| Claude Code | 46.9% | 46.9% | 17.8 m |

jcode and Codex CLI tied at 66.4% on GPT-5.6 Sol xhigh (11 jcode-only wins,
10 Codex-only wins); jcode reached 71.7% on Sol max. DataCurve's official
mini-SWE-agent sweep ranged from 45.4% at low effort to 72.7% at max.

The model dominates these results. At n=113, a 50% rate has a roughly
plus-or-minus 9.2-point 95% interval, wider than the matched-model harness
spread. DeepSWE's leaderboard holds mini-SWE-agent fixed and ranks models,
not harnesses. Report model, effort, timeout policy, median and maximum
duration, per-task rewards, and harness/dataset commits for any comparison.

## Historical Clanker results

These runs used the removed container integration. They are not reproducible
with the supported macOS/Windows binaries and must not be presented as current
platform validation.

Three tasks at sample seed 0, claude-sonnet-4-6 high, k=1:

| task | F2P | P2P | reward |
|---|---:|---:|---:|
| clack-async-autocomplete-options | 74/82 | 643/643 | 0 |
| happy-dom-deterministic-intersectionobserver | 13/14 | 9/9 | 0 |
| koota-deferred-mutation-buffer | 61/71 | 128/128 | 0 |

The four published Claude-model harnesses also scored zero on these three
tasks with Opus 4.8; such a small sample cannot distinguish harness quality.

The ten-task Sonnet concurrency gate recorded F2P 0.809, P2P 0.989 and reward
0.100. Its successful attempts cost an estimated $37.78 at the recorded
pricing. Pier deleted the initial eight failed attempts when retrying, so
`audit_job.py` rejects that job despite its final zero exceptions.

The recorded dataset checkout was `435ee89`, a descendant of jcode's
`8cae5984`. Differences across the 113 tasks were confined to `task.toml`
and `pre_artifacts.sh` (schema 1.1 to 1.3 and the collect hook), not task
instructions, tests, environments or solutions.

### What the first run found

An initial hardcoded 8192-token output cap truncated tool calls; the turn
finished without committing and all three submissions were empty. Raising the
cap and failing truncated turns rather than accepting them produced:

| | before | after |
|---|---|---|
| trials stopping on `length` | 3 of 3 | 0 of 3 |
| `git commit` calls | 0 | 2 per trial |
| `model.patch` | 0 bytes | 110 / 51 / 85 KB |
| turns taken | 75 / 42 / 21 | 71 / 74 / 128 |
| peak single-message output | 8192 | 17108 |
| F2P | 0.000 | 0.897 |

This is historical provenance, not a description of today's provider layer.
The enduring lesson is to check stop reasons, patch sizes and committed
changes before interpreting a zero as agent quality.
