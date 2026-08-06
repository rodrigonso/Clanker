# Feature parity: Clank vs. pi

Tracks how much of the upstream [`earendil-works/pi`](https://github.com/earendil-works/pi)
agent harness Clank currently reimplements.

| | |
|---|---|
| Comparison date | 2026-08-06 |
| pi ref | `bde81c84`, all workspace packages at `0.84.0` |
| Clank ref | `7d225ff` |
| Verification | Clank numbers measured from this tree; pi numbers read from source at the ref above |

Clank deliberately reimplements only the **memory-first core**. This document
is the running gap list — it is not a roadmap commitment.

**Legend:** ✅ functional parity · 🟡 partial · ❌ missing

## Scale

| | Clank | pi |
|---|---:|---:|
| Implementation source | 28,011 lines (`src/`) | 10 workspace packages, 9 published to npm |
| Test source | 10,701 lines (`tests/`) | per-package suites plus a private `evals` package |
| Offline test run | 33 suites, 1,581 checks, 0 failures | not measured here |
| Runtime dependencies | libcurl, C runtime, OS APIs | Node ≥ 22.19.0 or Bun, plus an npm dependency tree |
| Text providers | 2 | 39 |
| Wire protocols | 2 | 10 |
| Built-in tools | 7 | 7 |
| Slash commands | 14 | 27 plus dynamic |
| CLI flags | 14 long / 8 short | 38 long / 14 short |
| Community | personal project | 84.7k stars, 10.5k forks, 88 open issues |

## Upstream restructure (pi 0.84.0, released 2026-08-06)

Read the rest of this document with one caveat: pi shipped a breaking harness
and session rewrite on the comparison date itself.

- The **v2 lane-based `AgentHarness`** and the **v4 `Session` / `SessionStorage`
  / `SessionRepo`** APIs were promoted from the experimental entrypoint to the
  default package export. The legacy harness session model, the legacy JSONL
  repository, and the legacy in-memory repository APIs were removed.
- The promoted `AgentHarness` is a **compile-complete scaffold**. 23 methods
  work — `create`, `getLeafId`, `close`, and the configuration getter/setter
  pairs for model, thinking level, active tools, tools, resources, stream
  options, retry policy, compaction settings, steering mode, and follow-up
  mode. All 22 operational methods — `prompt`, `skill`, `promptFromTemplate`,
  `compact`, `navigateTree`, `resume`, `abort`, `steer`, `followUp`,
  `nextRun`, `cancelQueued`, `recordUsage`, `waitForIdle`, `runWhenIdle`,
  `peekAction`, `executeAction`, `runToCompletion`, `watch`, `lane`,
  `createLane`, `lanes`, `watchSession` — and the `hooks`/`events` registries
  currently reject with `HarnessNotImplemented` while durable execution lands.
- The shipping coding agent is unaffected: `packages/coding-agent` drives its
  own internal session runtime, not the public harness scaffold.

Two consequences. First, pi's **public** harness contract is mid-flight, so
"parity with pi's harness" is a moving target aimed at lanes, durable operation
records, and crash recovery. Second, the latency benchmark below cannot be
re-run against pi HEAD as written — see that section.

## Architecture mapping

| pi package | npm name | role | Clank | status |
|---|---|---|---|---|
| `ai` | `pi-ai` | 39 text providers, 10 wire APIs, auth, model/image catalogs | `src/llm`, `src/net`, `src/auth` | 🟡 |
| `agent` | `pi-agent-core` | agent loop, v2 harness scaffold, v4 lane sessions, compaction, queues, hooks | `src/agent`, `src/session` | 🟡 |
| `coding-agent` | `pi-coding-agent` | CLI, 7 tools, modes, resources, extensions, skills, trust | `src/app`, `src/tools`, `src/tui` | 🟡 |
| `tui` | `pi-tui` | differential renderer and component library | `src/term`, `src/ui`, `src/tui` | ✅ core |
| `session-backends/sqlite-node` | `pi-session-backend-sqlite-node` | SQLite `SessionRepo` implementation | JSONL and memory repositories in `src/session` | 🟡 |
| `protocol` | `pi-protocol` | protocol definitions shared by client/server | — | ❌ |
| `client` | `pi-client` | RPC client library | — | ❌ |
| `server` | `pi-server` | experimental multi-instance IPC/RPC supervisor | — | ❌ |
| `telemetry` | `pi-telemetry` | vendor-neutral telemetry contracts and adapters | — | ❌ |
| `evals` | private | model-judged behavioral evaluation harness | offline/unit tests only | ❌ |

## Core runtime / agent loop

| capability | status | notes |
|---|---|---|
| Event sequence `agent_start → turn_start → message_* → tool_* → turn_end → agent_end` | ✅ | Clank's 13 `AgentEventKind` values map 1:1 onto pi's 10 `AgentEvent` discriminants and add three: `RetryStart`, `RetryEnd`, `QueueUpdate` |
| Data model (text / thinking / tool_call / image) | ✅ | `llm/types.h` mirrors `packages/ai/src/types.ts` |
| Steering · follow-up · next-turn queues | ✅ | agent-owned thread-safe queues drained at pi's points; `queue_update` carries depths |
| Queue delivery modes (`all` / `one-at-a-time`) | ❌ | pi exposes `steeringMode` and `followUpMode`; Clank always drains the whole queue |
| Queue cancellation (`cancelQueued`) | ❌ | |
| Abort / cancellation | ✅ | shared abort flag, also short-circuits retry backoff |
| Parallel tool execution | ✅ | parallel by default; transient `ToolEnd` events follow completion order while persisted results follow source order, matching pi |
| Per-tool sequential override | ✅ | matches pi's `AgentTool.executionMode` |
| `beforeTool` gate · `afterToolCall` override | ✅ | field-by-field override of result content / `isError` / terminate |
| Tool live progress (`tool_execution_update`) | ✅ | `ToolUpdate` events; the `shell` tool emits an initial update then throttles to 100 ms, matching pi's `BASH_UPDATE_THROTTLE_MS` |
| Context transformation | 🟡 | Clank's `BeforeContext` hook can rewrite the message array per turn, covering pi's `transformContext`; pi additionally has `convertToLlm` and declaration-merged `CustomAgentMessages`, which Clank has no equivalent for |
| Per-turn state / stop hooks | ❌ | no equivalent for pi's `prepareNextTurn` or `shouldStopAfterTurn`; Clank's `AfterTurn` fires once per run and cannot stop the loop, and model/prompt/tools stay fixed for a harness run |
| Dynamic tool addition from a tool result | ❌ | pi's `AgentToolResult.addedToolNames` has no equivalent |
| Deferred / suspended runs | ❌ | pi's `RunOutcome.kind === "suspended"` + `DeferredHandle` + `write_deferred` records |
| Multi-lane sessions | ❌ | pi sessions carry named lanes with independent leaves; Clank has one branch head per session |
| Provider retries (`maxRetries` / backoff) | ✅ | separate provider-request and assistant-turn layers, both abortable and observable, default off |

Clank harness hooks: `BeforeRun`, `BeforeContext`, `BeforeProvider`,
`AfterProvider`, `BeforeTool`, `BeforeCompaction`, `BeforeNavigation`,
`AfterTurn` (8). pi's `AgentLoopConfig` exposes `convertToLlm` (required),
`transformContext`, `getApiKey`, `shouldStopAfterTurn`, `prepareNextTurn`,
`getSteeringMessages`, `getFollowUpMessages`, `toolExecution`,
`beforeToolCall`, `afterToolCall`, and its extension layer adds 33 further
event names.

## Latency benchmark

⚠️ **These numbers are pinned to pi `74caa2649f10ed71b4378ce69f5d9fbfd2466ca5`
and cannot be reproduced against pi HEAD.** pi 0.84.0 removed
`InMemorySessionStorage` and made `AgentHarness.prompt()` reject with
`HarnessNotImplemented`, so the fixture's entry points no longer exist. Treat
this as a historical result against the last behavior-complete public harness.
Re-basing the fixture onto `packages/coding-agent`'s internal runtime, or onto
the v2 harness once its operational methods land, is outstanding work.

Release measurements on the same Windows machine used Node 24.13.0, 50 warmup
runs, and 500 measured runs per case. pi was the durable exported
`AgentHarness` at the pinned ref with `InMemorySessionStorage`; Clank used its
`AgentHarness` with `MemoryRepository`. Values are median/p95 milliseconds.

| workload | Clank full run | pi full run | Clank tool → provider | pi tool → provider |
|---|---:|---:|---:|---:|
| sequential, 5 cycles | 0.169 / 0.203 | 0.542 / 0.825 | 0.015 / 0.019 | 0.023 / 0.037 |
| sequential, 20 cycles | 0.609 / 0.774 | 1.660 / 2.009 | 0.016 / 0.025 | 0.021 / 0.027 |
| sequential, 50 cycles | 1.626 / 1.878 | 4.108 / 4.579 | 0.020 / 0.027 | 0.022 / 0.029 |
| sequential, 100 cycles | 4.003 / 4.317 | 10.362 / 11.058 | 0.026 / 0.042 | 0.029 / 0.044 |
| parallel, 1 tool | 0.059 / 0.075 | 0.159 / 0.204 | 0.013 / 0.017 | 0.022 / 0.030 |
| parallel, 2 tools | 0.109 / 0.142 | 0.177 / 0.203 | 0.023 / 0.034 | 0.029 / 0.034 |
| parallel, 4 tools | 0.158 / 0.193 | 0.204 / 0.223 | 0.026 / 0.035 | 0.041 / 0.048 |
| parallel, 8 tools | 0.249 / 0.308 | 0.259 / 0.303 | 0.038 / 0.050 | 0.065 / 0.075 |
| parallel, 16 tools | 0.405 / 0.486 | 0.366 / 0.443 | 0.047 / 0.061 | 0.113 / 0.144 |

Clank was faster at every sequential depth and through four parallel tools,
with lower tool-to-provider medians throughout. At eight no-op tools its median
remained lower while p95 differed by 0.005 ms in pi's favor. Native worker
dispatch remained slower for the synthetic 16-no-op-tool batch (11% median, 10%
p95), while its tool-to-provider interval was still less than half of pi's.
Per-cycle cost stayed stable across the tested depths, with no quadratic curve.
Reproduce with `benchmarks/pi-agent-bench/README.md`.

## Footprint

Measured on this tree, Release build (`cmake -DCMAKE_BUILD_TYPE=Release`),
Windows x64, `CLANK_PRINT_MEM=1`:

| measurement | value |
|---|---:|
| Clank full faux run (libcurl init + agent + `shell` tool execution), peak RSS | **~8.9 MB** (8,868–8,900 KB over 5 runs) |
| Same workload, Debug build | ~11.0 MB |
| Bare `node -e` process RSS (Node 24.13.0), for reference | ~33 MB |
| `clank.exe` on disk (Release, libcurl statically linked) | ~0.97 MB |

The Node figure is an empty-process floor, not pi's working set: a loaded agent
adds the JS SDK object graphs and a UTF-16 transcript on top. pi publishes no
memory or startup numbers of its own, so no direct like-for-like comparison is
recorded here. The defensible claim is the order of magnitude, not a ratio
against a specific pi measurement.

## Providers & protocols

| capability | status | notes |
|---|---|---|
| Anthropic Messages provider + faux | ✅ | hand-rolled SSE build/parse, offline fixtures |
| OpenAI Responses provider | ✅ | public Responses API by key; ChatGPT subscription uses the Codex backend protocol, isolated behind separate fixtures |
| Remaining built-in providers | ❌ | pi's 39 provider entries include a separate `openai-codex`, so Clank's two providers cover three of them; the other ~36 are absent: Bedrock, Google, Vertex, Azure, GitHub Copilot, Groq, xAI, OpenRouter, DeepSeek, Mistral, Cerebras, Fireworks, Together, Moonshot, MiniMax, Qwen, Xiaomi, Z.ai, Nvidia, HuggingFace, Baseten, Cloudflare, Vercel AI Gateway, opencode, Radius, ant-ling, kimi-coding, and regional variants |
| Wire protocols | 🟡 | Clank implements `anthropic-messages` and `openai-responses` (plus the Codex backend variant). pi's `KnownApi` has 10: `openai-completions`, `mistral-conversations`, `openai-responses`, `azure-openai-responses`, `openai-codex-responses`, `anthropic-messages`, `bedrock-converse-stream`, `google-generative-ai`, `google-vertex`, `pi-messages` |
| Custom providers via config or extension | ❌ | pi supports `models.json` and extension-registered providers (Ollama, LM Studio, vLLM) |
| llama.cpp router integration | ❌ | pi has `/login llama.cpp` and `/llama` model management |
| Image generation | ❌ | pi has a separate `ImagesModels` surface and an `openrouter-images` API |
| Prompt caching | ✅ | Anthropic system/tool-prefix breakpoint plus the two most recent message groups |
| Thinking / reasoning streaming | 🟡 | Anthropic thinking deltas parsed; request-level effort control exposed. Clank's `ModelEffort` is `low, medium, high, xhigh, max, ultra`; pi's `ThinkingLevel` is `off, minimal, low, medium, high, xhigh, max`. No explicit thinking-budget control |
| Cross-provider message normalization | 🟡 | error/aborted assistant turns are omitted from provider replay and orphaned tool calls get a transient `No result provided` error; broader provider-specific normalization is absent |
| Model registry · discovery · capability metadata | 🟡 | provider registry, live `/v1/models` discovery merged over fallback metadata, effort and selectable-context profiles. Anthropic ships 3 fallback models plus 8 capability prefixes; OpenAI ships 5 |
| `--list-models` · pricing / cost · scoped models | ❌ | pi has `--list-models`, cost accounting, and `--models` / `/scoped-models` with Ctrl+P cycling |

## Tools

pi exposes 7 tool implementations. Its default coding bundle enables
`read`, `bash`, `edit`, `write`; a separate read-only bundle enables `read`,
`grep`, `find`, `ls`. Clank enables all seven equivalents by default and names
its command tool `shell`. (`edit-diff.ts` in pi is an internal helper for the
`edit` tool, not a separately registered tool.)

| pi tool | Clank | notes |
|---|---|---|
| `read` | 🟡 | `offset` / `limit`, verbatim text, head truncation, image content blocks. pi additionally supports an injectable `imageProcessor` with `autoResizeImages` (default on) and BMP conversion |
| `bash` | ✅ | named `shell`; timeout in seconds, process-tree kill, tail truncation, full-output temp log, and throttled live partial output all match |
| `write` | ✅ | creates parent dirs, overwrites, serialized per-file |
| `edit` | ✅ | exact + whitespace-tolerant fuzzy multi-edit, mutation lock, unified diff; NFKC omitted |
| `grep` | ✅ | ripgrep `--json`, with pi-compatible context / literal / ignore-case / glob behavior, limits, cancellation, hidden-file and `.gitignore` semantics, and local/PATH/download discovery |
| `find` | ✅ | native glob walk (not `fd`), gitignore-lite |
| `ls` | ✅ | case-insensitive sorted listings, dotfiles included |
| Tool allow/deny lists · default/read-only bundles | ❌ | pi has `--tools`, `--exclude-tools`, `--no-tools`, `--no-builtin-tools`; Clank always registers all seven |

Argument handling: both harnesses expose a per-tool `PrepareArguments` /
`prepareArguments` step. Clank additionally parses, schema-validates, and
coercion-normalizes tool arguments centrally before that hook runs, and fails
harness initialization on unsupported JSON Schema validation keywords.

## Auth

| capability | status | notes |
|---|---|---|
| API key (env + stored) | 🟡 | `ANTHROPIC_API_KEY` and `OPENAI_API_KEY` wired. pi resolves keys from `--api-key`, `auth.json`, env, and `models.json`, with `$VAR`, `${VAR}`, and `!shell-command` interpolation |
| Anthropic OAuth (PKCE + loopback callback) | ✅ | `sk-ant-oat` plus claude-code beta headers |
| OpenAI OAuth (ChatGPT subscription) | ✅ | loopback callback, account ID carried by the access token |
| Credential store (`~/.clank/auth.json`, restricted perms, atomic) · refresh | ✅ | resolved token and expiry cached in-process; expiry, login, provider change, and `/reload` invalidate the cache |
| Provider-generic OAuth config table | ✅ | `src/auth/` holds generic PKCE/exchange/refresh; only token application is per-provider |
| Other OAuth providers | ❌ | pi adds GitHub Copilot (device flow), xAI, OpenRouter (PKCE key minting), Radius |
| Bedrock ambient creds · Vertex ADC · Cloudflare | ❌ | pi supports `AWS_PROFILE`, ECS task roles, IRSA, `AWS_BEARER_TOKEN_BEDROCK`, and `gcloud` ADC |

## Sessions & persistence

pi's v4 session is a lane-based append-only JSONL log under
`~/.pi/agent/sessions/` with 7 entry types (`message`, `model_change`,
`thinking_level_change`, `active_tools_change`, `compaction`, `branch_summary`,
`custom`), 9 operational record types (`operation_started`, `abort_requested`,
`operation_finished`, `step_attempt`, `tool_started`, `queue_enqueued`,
`queue_cancelled`, `write_deferred`, `usage`), plus `lane` and `fact` log
items.

Clank's v2 schema under `~/.clank/sessions/v2/<project-key>/` has 9 record
kinds (`SessionStarted`, `Message`, `Model`, `Thinking`, `Tools`, `Compaction`,
`BranchLabel`, `SessionName`, `Leaf`). It intentionally persists only the tree
and model-visible state and keeps active turns and queues in memory.

| capability | status | notes |
|---|---|---|
| Append-only branchable message tree · leaf tracking | ✅ | |
| Resume / continue · cross-project resume | ✅ | exact global-ID resume, rollback-safe cwd/trust/context switching; `--continue` scoped to the project |
| Fork · tree checkout · branch labels · names | ✅ | |
| Rich session metadata browsing | 🟡 | current-project and all-project listings with times, model, counts, token categories, fork ancestry; no fuzzy/regex search, generated titles, pricing, or deletion UX |
| Durable operation records · crash recovery | ❌ | Clank's v2 deliberately dropped these on the basis that pi had simplified too. **That is no longer true** — pi's v4 re-introduced operation/step/tool/queue/deferred records and `findOpenOperations()` recovery. This is now a genuine divergence, not a shared simplification |
| Multi-lane sessions | ❌ | |
| Clone · import · export | ❌ | pi has `/clone`, `/import`, `/export` (HTML and JSONL) |
| SQLite backend | ❌ | pi ships a drop-in `SessionRepo` implementation |
| v1 migration | ❌ | intentional; v1 is not migrated, listed, or opened |

## Harness subsystems

| subsystem | status | notes |
|---|---|---|
| System prompt assembly (cwd, tool docs, overrides, `AGENTS.md`/`CLAUDE.md` discovery) | ✅ | global and project `SYSTEM.md` / `APPEND_SYSTEM.md`, root-to-cwd context discovery, CLI overrides, transactional reload; like pi, omits the date for prompt-cache stability |
| `AGENTS.override.md` | ❌ | pi allows a per-directory override file |
| Compaction | 🟡 | durable manual branch compaction with a fixed prompt. pi adds automatic thresholds (`reserveTokens` 16384, `keepRecentTokens` 20000), custom `/compact` instructions, split-turn two-phase summaries, cumulative file tracking, and branch summarization on tree navigation |
| Skills (`SKILL.md` discovery, frontmatter, prompt injection, `/skill:<name>`) | ❌ | |
| Prompt templates (`$1`, `$@`, `$ARGUMENTS`, `/templatename`) | ❌ | |
| Subagent / task delegation | ❌ | neither ships built-in subagents, but pi can gain them through its extension API. Clank has no extension seam, so this is unreachable rather than merely absent |

## CLI, modes & TUI

| capability | status | notes |
|---|---|---|
| Print mode (`-p`) · interactive TUI · plain REPL fallback | ✅ | |
| JSON mode (`--mode json`) · RPC mode (`--mode rpc`) | ❌ | pi streams NDJSON events and offers bidirectional stdio RPC with `pi-client` / `pi-server`. This is the main blocker for CI, automation, and SDK embedding |
| CLI flags | 🟡 | Clank has 14 long and 8 short. pi has 38 long and 14 short, adding provider, thinking, tools, models, extension, skill, template, theme, export, session-dir, verbose, offline, list-models, and tui-mode |
| Unknown-flag passthrough to extensions | ❌ | |
| TUI core: differential renderer, markdown, multi-line editor, reusable components, status line, scroll, selection, abort, `NO_COLOR` | ✅ core | |
| TUI modes | 🟡 | Clank is always alt-screen. pi offers `--tui-mode regular` (terminal-owned scrollback) and `fullscreen` |
| Selectors / dialogs | 🟡 | generic list/text modals power login, model/effort/context, session browsing and naming, tree, and trust. pi additionally has theme, settings, extension, and first-run surfaces |
| Slash commands | 🟡 | 14 built-ins versus pi's 27, plus pi's dynamic skill, template, and extension commands. Clank lacks `/new`, `/clone`, `/import`, `/export`, `/share`, `/copy`, `/hotkeys`, `/changelog`, `/settings`, `/scoped-models`, `/llama`, `/logout` |
| Editor features | 🟡 | bracketed paste, history, selection, mouse, OSC 52 copy. pi adds `@` fuzzy file search, Tab path completion, `!command` / `!!command` shell input, `Ctrl+G` external editor, undo stack, and kill ring |
| Images | ❌ | pi renders inline images via Kitty and iTerm2 protocols and accepts image paste |
| LaTeX rendering · syntax highlighting | ❌ | pi ships `renderLatex` and highlighted output |
| Themes · configurable keybindings | ❌ | pi has `--theme`, `/settings`, and a `KeybindingsManager` |

## Platform features

| capability | status | notes |
|---|---|---|
| Extensions | ❌ | pi's extension API exposes 33 event names and 11 `register*` methods spanning tools, slash commands, shortcuts, CLI flags, providers, message/entry renderers, markdown transformers, UI widgets, custom editor/footer/header, full-screen UI, autocomplete providers, an inter-extension event bus, and custom session entries. Clank has no plugin seam at all |
| MCP | ❌ (both) | pi excludes MCP by design; it is reachable through extensions |
| Trust / project trust · `settings.json` | 🟡 | hierarchical locked/atomic project trust and `defaultProjectTrust`; no `keybindings.json` |
| Selection / clipboard copy | ✅ | transcript and composer; `Ctrl+C` sends OSC 52 |
| Export HTML · import · share-as-gist | ❌ | |
| Experimental server (multi-instance IPC) | ❌ | |
| Telemetry | ❌ | pi ships a vendor-neutral telemetry package with typed schemas and conformance tests |
| Sandboxing | ❌ (both) | neither sandboxes natively; pi documents Docker, micro-VM, and policy-sandbox patterns and ships a `sandbox` extension example |
| Permission / approval prompts | ❌ (both) | pi explicitly ships no permission system; both run with the caller's privileges. Clank's `BeforeTool` and pi's `beforeToolCall` both allow a programmatic gate |
| Supply-chain hardening | 🟡 | Clank vendors a checksummed libcurl archive and has almost no dependency surface. pi pins exact deps, enforces a minimum release age, guards lockfiles, and audits signatures in CI |

## Bottom line

Clank holds close on the **memory-first execution core**: the data model,
high-level loop semantics, parallel tool ordering, live tool progress, queue
plumbing, Anthropic and OpenAI SSE plus OAuth, durable branchable sessions,
manual compaction, prompt/context/trust assembly, a full-screen differential
TUI, and equivalents for all seven pi tools. It does so in 28k lines with no
C++ standard library, runs the documented faux workload at ~8.9 MB peak RSS,
and was measurably faster than the pinned pi harness across every sequential
depth tested.

The gaps fall into three buckets:

1. **Breadth.** 2 providers against 39, 2 wire protocols against 10, no model
   catalog metadata beyond capability hints, no cost accounting. This is the
   largest raw gap and the most mechanical to close.
2. **Surfaces.** No `--mode json` and no RPC means Clank cannot be driven
   programmatically, which rules out CI use and SDK embedding. This is a small
   amount of work with a disproportionate payoff.
3. **Ecosystem.** Extensions, skills, prompt templates, themes, keybindings,
   import/export/share, telemetry. Clank has no extension seam, so everything
   downstream of it — including subagents — is unreachable rather than merely
   unimplemented.

Two items changed character since the previous revision of this document.
Clank's session layer dropped durable operation records on the reasoning that
pi had done the same; pi's v4 has since re-introduced them alongside lanes and
crash recovery, so that is now a real divergence to decide on deliberately.
And the latency benchmark, while still a valid historical result, no longer has
a runnable upstream counterpart.

## Refreshing this report

```sh
# clone the current pi and re-survey
git clone --depth 1 https://github.com/earendil-works/pi.git /tmp/pi

# pi tools:        packages/coding-agent/src/core/tools/index.ts
# pi providers:    packages/ai/src/providers/all.ts
# pi wire APIs:    packages/ai/src/types.ts (KnownApi)
# pi loop:         packages/agent/src/agent-loop.ts
# pi loop config:  packages/agent/src/types.ts (AgentLoopConfig, AgentEvent)
# pi harness:      packages/agent/src/harness/agent-harness.ts
# pi harness doc:  packages/agent/docs/harness-v2.md
# pi sessions:     packages/agent/src/harness/session/types.ts
# pi skills:       packages/agent/src/harness/skills.ts
# pi extensions:   packages/coding-agent/src/core/extensions/types.ts
# pi cli flags:    packages/coding-agent/src/cli/args.ts
# pi slash cmds:   packages/coding-agent/docs/usage.md
# pi tui exports:  packages/tui/src/index.ts
# pi changelog:    packages/agent/CHANGELOG.md
```

Re-measure Clank's side from a Release build:

```powershell
cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel --target clank clank_tests
.\build-rel\clank_tests.exe
$env:CLANK_PRINT_MEM=1; .\build-rel\clank.exe --faux -p "read README.md"
```

Update the metadata table (date, pi ref, Clank ref) when re-running.
