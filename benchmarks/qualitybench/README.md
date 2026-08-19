# qualitybench

[`agentbench/`](../agentbench/) measures what a harness *costs*. This measures
what it *does*: task success on a public coding benchmark, with the model held
fixed so the harness is the only thing that varies.

`clanker_agent.py` exposes Clanker as an agent to [Pier](https://github.com/datacurve-ai/pier)
(DeepSWE) and [Harbor](https://github.com/harbor-framework/harbor)
(Terminal-Bench). Both resolve a custom agent from an import path and expose the
same `BaseInstalledAgent` surface, so one file serves both. **Only the DeepSWE
path has actually been run**; the Harbor path is written to the same API but
untested.

## Setup

Clanker is a compiled binary with no package registry to install from, so the
adapter uploads it into the task container. **It must be built against the task
image's glibc, not your host's.** DeepSWE images are Debian 12 bookworm (glibc
2.36); a binary built on Ubuntu 24.04 (glibc 2.39) fails at agent setup with

```
/usr/local/bin/clanker: /lib/x86_64-linux-gnu/libc.so.6: version `GLIBC_2.38' not found
```

`build_bookworm.py` builds a compatible binary in a Modal sandbox, so it needs
no local Docker:

```bash
python build_bookworm.py ~/clanker-src -o ~/clanker-bookworm
export CLANKER_BIN=$HOME/clanker-bookworm
```

Bookworm ships `libssl-dev`, so that build links the image's own OpenSSL and
needs **no** `clanker_libs` upload. Check any other binary with `ldd` and
`objdump -T clanker | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1`; pass
`--ak clanker_libs=a.so,b.so` only if it needs libraries the image lacks.

Then the harness itself:

> **Install Pier from git, not PyPI.** The published `datacurve-pier` 0.3.0
> predates task `schema_version = "1.3"`: its `VerifierConfig` has no `collect`
> field, so it silently ignores the `[[verifier.collect]]` block that produces
> `model.patch`. The verifier grades an empty submission and **every task scores
> 0**, the oracle included. Measured: oracle F2P 0/20 on 0.3.0 versus 20/20 on
> git HEAD, same task.

```bash
uv tool install --force "git+https://github.com/datacurve-ai/pier"
git clone https://github.com/datacurve-ai/deep-swe

modal token new                # Pier refuses to start without one
export ANTHROPIC_API_KEY=...   # or OPENAI_API_KEY, matching --model
```

DeepSWE tasks declare `memory_mb = 8192` and `storage_mb = 20480` each, so on a
small machine either keep `-n 1` or use `--env modal`.

## Run

Three gates before committing to a full sweep. Each buys information the next
stage depends on; skipping one means finding the problem five hours and a large
API bill in. `run_benchmark.py` owns the safe defaults for all three:

- Clanker retries transient provider requests in place (up to eight retries and a
  60-second delay), preserving the task state and prompt cache.
- Pier trial retries are fixed at zero. Pier deletes the failed trial directory
  before retrying, including `clanker.stderr`, NDJSON and the exception body.
- Agent concurrency defaults to four. The oracle, which makes no model calls,
  defaults to eight.
- The full sweep refuses to start until its API-cost warning is acknowledged.

```bash
cd benchmarks/qualitybench
python3 run_benchmark.py oracle
```

**Gate 1 — oracle over all 113.** Free of model calls: the oracle applies each
reference patch and runs the verifier. Every task MUST score `reward 1.0`.
Anything less is a broken harness, task, or environment rather than an agent
result. Modal currently verifies 112/113: Prometheus's ten IPv6 QueryLog cases
skip under gVisor with `ip version not supported`, and Pier counts the skips as
P2P failures. Confirm that one task under Docker before calling the gate clean.

**Gate 2 — concurrency shakeout.** Ten tasks at the concurrency you intend to
use, on the cheaper model. What matters is not the score but the
`Exceptions` column: it should be 0. Anything there is rate limiting or Modal
capacity, and it will only get worse at 113.

```bash
python3 run_benchmark.py canary
```

**Gate 3 — the publishable number.** All 113, matched to a published arm.
`--agent-timeout-multiplier` reproduces jcode's natural-completion policy;
without it each task's own 5400 s cap truncates the long trials. Based on the
measured canary and current Opus pricing, budget roughly **$700-$900 in
Anthropic API credits**, plus Modal compute.

```bash
python3 run_benchmark.py full --confirm-full-sweep
```

Override the conservative defaults with `--concurrency`, `--environment`,
`--model`, `--max-retries` or `--retry-max-delay-ms`. Results land in
`jobs/<timestamp>/`, one directory per trial holding `agent/clanker.ndjson`,
`agent/clanker.stderr`, `artifacts/model.patch` and `verifier/reward.json`;
`pier view jobs` browses them.

Audit every job before reading its score:

```bash
python3 audit_job.py jobs/<timestamp>
```

The audit fails on incomplete jobs, exceptions, destructive Pier retries,
missing patches, missing event logs, and truncated final messages. Empty
patches are legitimate scored failures and produce warnings. The audit also
counts Clanker's in-place request retries and estimates API cost from the actual
input, cache-read, cache-write and output tokens. A failed audit means the
score is not benchmark evidence, even if Pier's final table has zero
exceptions.

Two things bite only at full scale:

- **Timeout policy is a methodology choice, not a detail.** DeepSWE tasks set
  `timeout_sec = 5400`, and jcode's *untimed* column comes from letting agents
  run to natural completion and applying that cutoff afterwards — 18 of their
  113 Opus trials ran over it, 8 of which passed. Run with the multiplier and
  report both columns, or run without it and compare only against the timed
  column. Expect trials far longer than anything seen at three tasks: our
  longest was 22 minutes against a 37.5-minute median and a 188-minute maximum
  in jcode's Opus run.
- **Rate limits, not Modal, set the concurrency ceiling.** The measured ten-task
  canary averaged 8.1M cache-read, 132.6k cache-write and 56.5k output tokens per
  task. Clanker honors provider retry headers and emits `retry_start`/`retry_end`
  events; the adapter widens its request budget to eight retries capped at one
  minute. Keep concurrency at four unless a clean canary supports raising it.
  Never use Pier's `-r`/`--max-retries`: it restarts the whole task and erases
  the failed attempt.

The successful attempts in the ten-task Sonnet canary cost an estimated
**$37.78**. Extrapolating its usage at current Opus rates puts the 113-task arm
near $700 before abnormal retries, so the runner states a $700-$900 planning
range. Provider prices can change; verify them before acknowledging the full
sweep. Job logs run around 1.2 MB per trial, or roughly 140 MB for all 113.

Adapter checks run with no harness installed:

```bash
python3 test_clanker_agent.py
python3 test_benchmark_flow.py
```

## Reading the score

`F2P` is the fail-to-pass tests the change had to fix; `P2P` is the
pass-to-pass regression guard; `reward` is **all-or-nothing** — 1.0 only when
every F2P test passes and nothing regresses. An 88% F2P rate still scores
`reward 0`, so **do not quote F2P next to anyone else's headline number**:
published DeepSWE figures are resolve rates (mean reward), not test rates.

Grading is on **committed** work. The collect hook captures
`git diff <base_commit> HEAD`, so an uncommitted edit scores zero no matter how
correct it is.

## Reference numbers

Published by [jcode](https://jcode.sh/benchmarks/deepswe), DeepSWE v1.1, 113
tasks, dataset commit `8cae5984`. The matched-harness arm — same model, same
effort, same `k`, only the harness changes:

| harness (Claude Opus 4.8, high, k=1) | untimed | at the 5400 s cutoff | median |
|---|---:|---:|---:|
| OpenCode | 52.2% | 52.2% | 16.1 m |
| mini-SWE-agent | 51.3% | 51.3% | 12.1 m |
| jcode | 50.4% | 43.4% | 37.5 m |
| Claude Code | 46.9% | 46.9% | 17.8 m |

Other arms: jcode and Codex CLI tie at 66.4% on GPT-5.6 Sol xhigh (paired: 11
jcode-only wins against 10 codex-only, which is a coin flip); jcode reaches
71.7% on Sol max; DataCurve's official mini-SWE-agent sweep runs 45.4% at low
effort to 72.7% at max.

Three things follow, and they set expectations for anything measured here:

- **The model dominates.** Four harnesses on one model span 5.3 points; changing
  the model moves three to four times further. At n=113 the 95% interval on a
  50% rate is ±9.2 points, so that entire harness spread sits inside the noise
  floor.
- **There is no harness leaderboard to enter.** DeepSWE's own README states all
  leaderboard scores come from Pier running `mini-swe-agent` — it ranks models
  with the harness held fixed. jcode's page is self-published for exactly that
  reason, and it is the format to copy: report untimed *and* post-hoc 5400 s,
  median and max agent seconds, per-task rewards, harness commit, dataset
  commit.
- **Nobody publishes cost or tokens.** No harness in that table reports either,
  and jcode's own entries set `cost_usd: null`. DeepSWE cannot carry Clanker's
  footprint argument; that stays in `agentbench/`.

So the realistic goal here is not a win but a floor: evidence that Clanker
resolves tasks in the same band as Claude Code and OpenCode, so the footprint
claim rests on a harness that is not quietly worse.

## Results so far

Three tasks at `--sample-seed 0`, Clanker on claude-sonnet-4-6 high, k=1:

| task | F2P | P2P | reward |
|---|---:|---:|---:|
| clack-async-autocomplete-options | 74/82 | 643/643 | 0 |
| happy-dom-deterministic-intersectionobserver | 13/14 | 9/9 | 0 |
| koota-deferred-mutation-buffer | 61/71 | 128/128 | 0 |

**This sample cannot distinguish anything.** All four published harnesses score
0 on these same three tasks with Opus 4.8, a stronger model than ours — they
are simply hard for Claude models. (They are not impossible: jcode solves two of
the three on Sol xhigh.) A real number needs the full 113.

The ten-task concurrency gate on the same Sonnet model completed with F2P
0.809, P2P 0.989 and reward 0.100 (one task resolved). Its final artifacts were
healthy, but Pier retried the initial eight-task wave and deleted all eight
failed attempts. `audit_job.py` therefore rejects that job despite its final
`Exceptions: 0`. A subsequent eight-way one-turn probe succeeded 8/8 with
Clanker retries disabled, ruling out a persistent eight-request account limit;
the first-wave error itself is unrecoverable.

Our checkout is `435ee89`, a descendant of jcode's `8cae5984`. The only
difference across all 113 tasks is `task.toml` and `pre_artifacts.sh` — schema
1.1 to 1.3 plus the `verifier.collect` hook. No `instruction.md`, `tests/`,
`environment/` or `solution/` file differs, so the tasks themselves are
identical.

## What the first run found

The first real run scored **F2P 0.000** with `P2P 260/260` and a **0-byte
`model.patch`** on every task — the signature of an agent that never committed.
It was a bug in Clanker, which is the benchmark earning its keep on day one.

Every trial ended with `stopReason: length` and `usage.output` of exactly
**8192**, the hardcoded Anthropic output cap. Context was 39k–99k of 1M, so this
was the *output* limit, not context pressure. None of the steps look like a bug
alone:

1. The model starts writing a large file; the response exceeds 8192 tokens.
2. Anthropic returns `stop_reason: max_tokens` without closing the in-flight
   `tool_use` block, so Clanker materializes a message with text and **no tool
   calls**.
3. `AgentCheckpoint_ContinueTurn` continues only while tools are running, so an
   empty batch reads as *"the model is done"*.
4. The run exits reporting success, uncommitted. `git diff <base> HEAD` is
   empty, so the task scores 0.

Both halves were fixed: `Anthropic_Model()` and the fallback models now cap
output at 64000, matching pi, which sends the real per-model limit from its
catalog; and `StopReason::Length` now fails the turn instead of ending it clean,
mirroring pi's refusal to execute tool calls from a truncated message. Same
three tasks, same seed, before and after:

| | before | after |
|---|---|---|
| trials stopping on `length` | 3 of 3 | **0 of 3** |
| `git commit` calls | 0 | 2 per trial |
| `model.patch` | 0 bytes | 110 / 51 / 85 KB |
| turns taken | 75 / 42 / 21 | 71 / 74 / 128 |
| peak single-message output | 8192 (the cap) | **17108** |
| **F2P** | **0.000** | **0.897** |

That 17108-token message is the direct evidence: it is the write the old build
truncated. The lesson for anyone reading a number out of this directory: **an
agent that scores 0 is not necessarily an agent that answered badly.** Check
`stopReason`, the patch size, and whether `git commit` appears in the tool calls
first.

## How it maps onto Clanker

- **Instruction** — `clanker --mode json --approve -p "<instruction>"`, which runs
  fully headless: no tool-approval prompt, and `--approve` covers project prompt
  resources only.
- **Model and effort** — Clanker's `-m` overrides the model id but *not* the
  provider, so an OpenAI model id would silently point at the Anthropic
  provider. Both, plus effort, are written to `~/.clanker/settings.json` instead.
  Efforts: `low`, `medium`, `high`, `xhigh`, `max`, `ultra`.
- **Accounting** — token counts come from `record.payload.message.usage` on
  assistant messages. Clanker does not price requests, so cost is left unreported
  rather than reported as zero.
- **Network** — DeepSWE runs the agent with `network_mode = "no-network"`, and
  Pier opens an egress path only for domains an agent declares in
  `network_allowlist()`. The adapter declares the provider API domain plus any
  `*_BASE_URL` override; Pier fronts the container with an authenticated Squid
  proxy and injects `HTTPS_PROXY`, which libcurl honours for free. The inherited
  empty allowlist would fail every task with no route to host.
- **Offline tools** — `CLANKER_OFFLINE=1` is set and `ripgrep` installed at image
  build, so the `grep` tool never reaches for a download mid-trial.
- **Install vs upload** — the `apt-get` step is baked into the image, but the
  binary is uploaded in `setup()`, not `install()`: Pier skips `install()`
  entirely once the spec is baked in, so an upload placed there never runs.

## Caveats

- **No `-C`/`--cwd`.** The working directory is set on `exec` instead, left
  unset so the task image's own `WORKDIR` applies; override with
  `--ak workdir=...`.
- **No `--version`.** The adapter records `sha256:<12 hex>` of the uploaded
  binary, which is better provenance for a benchmark anyway.
- **The binary must match the container's libc.** The `--help` smoke test in
  `setup()` makes a mismatch fail fast rather than mid-trial; that is deliberate.
- **Scores are dominated by the model.** Any number produced here is meaningless
  unless the comparison arm ran the same model at the same effort.
