# Feature parity: Clank vs. pi

Tracks how much of the upstream [`earendil-works/pi`](https://github.com/earendil-works/pi)
agent harness Clank currently reimplements.

| | |
|---|---|
| Comparison date | 2026-07-30 |
| pi ref | `74caa2649f10ed71b4378ce69f5d9fbfd2466ca5` |
| Clank ref | current latency-parity working tree |

Clank deliberately reimplements only the **memory-first core**. This
document is the running gap list — it is not a roadmap commitment.

**Legend:** ✅ functional parity · 🟡 partial · ❌ missing

## Architecture mapping

The surveyed pi workspace has seven package roots: the four public core
surfaces plus behavioral evals, an experimental server, and a SQLite session
adapter. Clank collapses the core surfaces into its module tree.

| pi package | role | Clank | status |
|---|---|---|---|
| `ai` | 38 built-in text providers, 10 text APIs, auth, model/image catalogs | `src/llm`, `src/net`, `src/auth` | 🟡 |
| `agent` | loop plus durable harness, sessions, compaction, queues, hooks | `src/agent`, `src/session` | 🟡 |
| `coding-agent` | CLI, 7 tool implementations, modes, resources, extensions, trust | `src/app`, `src/tools`, `src/tui` | 🟡 |
| `tui` | differential renderer and component library | `src/term`, `src/ui`, `src/tui` | ✅ core |
| `storage/sqlite-node` | SQLite session repository and materialized views | JSONL and memory repositories in `src/session` | 🟡 |
| `server` | experimental multi-instance IPC/RPC supervisor | — | ❌ |
| `evals` | model-judged behavioral evaluation harness | offline/unit tests only | ❌ |

## Core runtime / agent loop

| capability | status | notes |
|---|---|---|
| Event sequence `agent_start → turn_start → message_* → tool_* → turn_end → agent_end` | ✅ | matches pi's high-level order |
| Data model (text / thinking / tool_call / image) | ✅ | `types.h` mirrors `types.ts` |
| Steering · follow-up · `beforeTool` gate | ✅ | agent-owned thread-safe queues drained at pi's points |
| Abort / cancellation | ✅ | shared abort flag (also short-circuits retry backoff) |
| **Parallel tool execution** | ✅ | execution and transient `ToolEnd` events follow completion order; persisted tool results follow assistant/source order, matching pi |
| `afterToolCall` hook | ✅ | field-by-field override of result content / isError / terminate in finalize |
| `nextTurn` queue · `queue_update` events | ✅ | agent owns steer/followUp/nextTurn queues + thread-safe push API; `queue_update` carries queue depths |
| Context conversion / transformation | ❌ | no equivalents for pi's custom `AgentMessage`, `convertToLlm`, or `transformContext` hooks |
| Per-turn state / stop hooks | ❌ | no equivalents for pi's `prepareNextTurn` or `shouldStopAfterTurn`; model, prompt, and tools stay fixed for a harness run |
| Provider retries (`maxRetries` / backoff) | ✅ | separate provider-request and assistant-turn layers; both are abortable, observable, and default off unless explicitly configured |

## Latency-parity benchmark

Release measurements on the same Windows machine used Node 24.13.0, 50 warmup
runs, and 500 measured runs per case. Pi is the durable exported
`AgentHarness` at the pinned ref above with `InMemorySessionStorage`; Clank uses
its `AgentHarness` with `MemoryRepository`. Values are median/p95 milliseconds.

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

Clank is faster at every sequential depth and through four parallel tools,
with lower tool-to-provider medians throughout. At eight no-op tools its median
remains lower while p95 differs by 0.005 ms in pi's favor. Native worker
dispatch remains slower for the synthetic 16-no-op-tool batch (11% median,
10% p95), while its tool-to-provider interval is still less than half of pi's.
Per-cycle cost stays stable across the tested depths, with no quadratic curve.
Reproduce both JSON reports with `benchmarks/pi-agent-bench/README.md`.

## Providers & protocols

| capability | status | notes |
|---|---|---|
| Anthropic Messages provider + faux | ✅ | hand-rolled SSE build/parse, offline unit test |
| 37 other built-in text providers (OpenAI, Google, Vertex, Bedrock, GitHub Copilot, Groq, xAI, OpenRouter, …) | ❌ | |
| 9 other text APIs plus OpenRouter image generation | ❌ | OpenAI completions/responses/codex, Google, Vertex, Bedrock, Mistral, Azure Responses, and pi-messages are absent |
| Prompt caching (`cache_control`) | ✅ | Anthropic system/tool-prefix breakpoint plus the two most recent message groups |
| Thinking / reasoning streaming | 🟡 | Anthropic thinking deltas parsed; supported models expose request-level effort control, but explicit thinking-mode/budget control remains absent |
| Cross-provider message normalization (`transform-messages`) | 🟡 | error/aborted assistant turns are omitted from provider replay, and orphaned tool calls receive transient `No result provided` error results; broader provider-specific normalization is absent |
| Model registry · `--list-models` · pricing/cost · scoped/image models | 🟡 | provider registry + live Anthropic `/v1/models` discovery, capability metadata, effort selection, fixed Anthropic context display, and generic selectable context profiles; no pricing, list flag, scoped models, or image catalog |

## Tools

pi exposes seven built-in tool implementations but enables only `read`, `bash`,
`edit`, and `write` in its default coding bundle. Clank enables all seven
equivalents by default and names its command tool `shell`.

| pi tool | Clank | notes |
|---|---|---|
| `read` | ✅ | `offset` / `limit`, verbatim text, pi-style head truncation, image content blocks; no image auto-resize |
| `bash` | 🟡 | Clank's equivalent is named `shell`; timeout in seconds, process-tree kill, tail truncation, and full-output temp log match, but live output updates do not |
| `write` | ✅ | creates parent dirs, overwrites, serialized per-file |
| `edit` | ✅ | exact + whitespace-tolerant fuzzy multi-edit, mutation lock, unified diff; NFKC omitted |
| `grep` | ✅ | invokes ripgrep `--json`, with Pi-compatible context / literal / ignore-case / glob behavior, limits, cancellation, hidden-file + `.gitignore` semantics, and local/PATH/download discovery; always enabled in Clank |
| `find` | ✅ | native glob walk (not `fd`), gitignore-lite; always enabled in Clank |
| `ls` | ✅ | case-insensitive sorted listings, dotfiles included; always enabled in Clank |
| Tool allow/deny lists · default/read-only bundles | ❌ | Clank always registers all seven tools |

## Auth

| capability | status | notes |
|---|---|---|
| API key (env + stored) | ✅ | |
| Anthropic OAuth (PKCE + loopback callback) | ✅ | `sk-ant-oat` → `******` claude-code beta headers |
| Credential store (`~/.clank/auth.json`, 0600, atomic) · refresh | ✅ | resolved tokens and OAuth expiry are cached in-process; expiry, login, provider changes, and `/reload` invalidate or replace the cache |
| Other OAuth providers (GitHub Copilot device flow; OpenAI Codex, Kimi, OpenRouter, Radius, xAI) | ❌ | |
| Cloudflare auth · Google Vertex ADC · Bedrock ambient creds | ❌ | |
| Multi-provider env-key map | ❌ | only `ANTHROPIC_API_KEY` wired |

## Harness subsystems

| subsystem | status | notes |
|---|---|---|
| System prompt assembly (cwd, tool docs, overrides, `AGENTS.md`/`CLAUDE.md` project context) | ✅ | active-tool prompt, global/project `SYSTEM.md` and `APPEND_SYSTEM.md`, root-to-cwd context discovery, CLI overrides, and transactional reload; like current pi, omits the date for prompt-cache stability |
| Prompt templates (`$1`, `$@`, `$ARGUMENTS`) | ❌ | |
| Sessions / persistence (JSONL/SQLite, resume, fork/clone, branch, tree, import/export, naming) | 🟡 | incompatible message-oriented v2 JSONL, incremental in-memory index/state, exact global-ID resume, rollback-safe cross-project switching, tree checkout, forks, labels, and names; v1 is not migrated and there is no durable operation recovery, import/export, or clone |
| Compaction (auto + manual, custom instructions, branch summarization) | 🟡 | durable manual branch compaction; no automatic threshold policy or custom prompt |
| Skills (discovery, frontmatter, `<available_skills>` injection, `/skill`) | ❌ | |
| Subagent / task delegation | ✅ | neither harness ships built-in subagents; pi expects extensions/packages for this |

## CLI, modes & TUI

| capability | status | notes |
|---|---|---|
| Print mode (`-p`) · interactive TUI · plain REPL fallback | ✅ | |
| JSON output mode · RPC (JSONL) mode | ❌ | |
| CLI flags | 🟡 | adds system/append prompt, context-disable, and trust overrides to model/print/login/logout/session flags; pi's provider, thinking, tool/resource, export, and mode breadth remains absent |
| Interactive queue UX | 🟡 | steering during a run works; no distinct follow-up shortcut, queue retrieval, or one-at-a-time/all delivery settings |
| TUI core: differential renderer, markdown, multi-line editor, reusable components, status line, scroll, selection, abort, `NO_COLOR` | ✅ core | |
| TUI selectors/dialogs (model / session / theme / settings / login / oauth / extension / thinking / trust / diff / image / first-run) | 🟡 | generic list/text modals power login, model/effort/context, rich current/all-project session browsing and naming, tree, and project-trust flows |
| Rich editor/media features | 🟡 | bracketed paste, history, selection, and mouse are present; no path/file autocomplete, inline image rendering/paste, or external editor |
| Slash commands | 🟡 | 14 built-ins, including `/name`, `/trust`, and `/reload`, versus pi's 22 plus dynamic extension/skill/template commands |
| Themes · configurable keybindings | ❌ | |

Session browsing intentionally remains simpler than pi: fuzzy/regex search,
generated titles, pricing, deletion UX, and a derived metadata database/sidecar
are deferred. Canonical JSONL records remain the only durable source.

## Platform features

| capability | status | notes |
|---|---|---|
| Extensions / Pi packages (tools, commands, flags, renderers, providers, UI hooks, MCP, install/update/config) | ❌ | |
| Trust / project-trust · `settings.json` · `keybindings.json` | 🟡 | hierarchical locked/atomic project trust and `defaultProjectTrust` settings; no keybindings |
| Selection / clipboard copy | ✅ | transcript and composer; `Ctrl+C` sends OSC 52 |
| Export HTML · import · share-as-gist | ❌ | |
| Experimental server (multi-instance IPC: serve/spawn/list/status/stop/rpc) | ❌ | |
| Telemetry / resource diagnostics | ❌ | |

## Bottom line

Clank remains close on the **memory-first execution core**: data model,
high-level loop semantics, Anthropic SSE/OAuth, durable sessions and manual
compaction, a full-screen differential TUI, and equivalents for all seven pi
tool implementations. Parallel completion ordering now matches pi; live tool
progress depends on individual tools emitting updates.

The completed prompt/context/trust work closes one of the largest harness gaps.
The remaining gaps are primarily (a) **breadth** — providers, APIs, auth modes,
model metadata, and CLI modes — and (b) the **coding-agent ecosystem** —
dynamic context transforms, skills, prompt templates, extensions/packages,
themes/keybindings, and import/export/share.

## Refreshing this report

```sh
# clone the current pi and re-survey
git clone --depth 1 https://github.com/earendil-works/pi.git /tmp/pi
# pi tools:      packages/coding-agent/src/core/tools/index.ts
# pi providers:  packages/ai/src/providers/all.ts
# pi protocols:  packages/ai/src/types.ts (KnownApi)
# pi loop:       packages/agent/src/agent-loop.ts
# pi harness:    packages/agent/src/harness/agent-harness.ts
# pi cli flags:  packages/coding-agent/src/cli/args.ts
# pi slash cmds: packages/coding-agent/src/core/slash-commands.ts
# pi server:     packages/server/src/cli.ts
```

Update the metadata table (date + pi ref) when re-running.
