# Feature parity: Clank vs. pi

Tracks how much of the upstream [`earendil-works/pi`](https://github.com/earendil-works/pi)
agent harness Clank currently reimplements.

| | |
|---|---|
| Comparison date | 2026-07-28 |
| pi ref | `027a584` (pi-coding-agent `0.82.1`) |
| Clank ref | `2a85589` + current prompt-context working tree |

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
| **Parallel tool execution** | 🟡 | execution and persisted results match pi's concurrency/source-order semantics; Clank buffers `ToolEnd` until the whole batch finishes and emits no live `ToolUpdate`, while pi emits updates and completion-order end events |
| `afterToolCall` hook | ✅ | field-by-field override of result content / isError / terminate in finalize |
| `nextTurn` queue · `queue_update` events | ✅ | agent owns steer/followUp/nextTurn queues + thread-safe push API; `queue_update` carries queue depths |
| Context conversion / transformation | ❌ | no equivalents for pi's custom `AgentMessage`, `convertToLlm`, or `transformContext` hooks |
| Per-turn state / stop hooks | ❌ | no equivalents for pi's `prepareNextTurn` or `shouldStopAfterTurn`; model, prompt, and tools stay fixed for a harness run |
| Provider retries (`maxRetries` / backoff) | ✅ | retryable-error classifier + capped exponential backoff in the stream loop; default on (maxRetries=3, 2s base, 60s cap), abortable |

## Providers & protocols

| capability | status | notes |
|---|---|---|
| Anthropic Messages provider + faux | ✅ | hand-rolled SSE build/parse, offline unit test |
| 37 other built-in text providers (OpenAI, Google, Vertex, Bedrock, GitHub Copilot, Groq, xAI, OpenRouter, …) | ❌ | |
| 9 other text APIs plus OpenRouter image generation | ❌ | OpenAI completions/responses/codex, Google, Vertex, Bedrock, Mistral, Azure Responses, and pi-messages are absent |
| Prompt caching (`cache_control`) | ✅ | Anthropic system/tool-prefix breakpoint plus the two most recent message groups |
| Thinking / reasoning streaming | 🟡 | Anthropic thinking deltas parsed; no thinking-level control |
| Cross-provider message normalization (`transform-messages`) | 🟡 | error/aborted assistant turns are retained in the transcript but omitted from provider replay; broader provider-specific normalization is absent |
| Model registry · `--list-models` · pricing/cost · scoped/image models | 🟡 | provider registry + live Anthropic `/v1/models` discovery; no pricing, list flag, scoped models, or image catalog |

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
| Credential store (`~/.clank/auth.json`, 0600, atomic) · per-request refresh | ✅ | |
| Other OAuth providers (GitHub Copilot device flow; OpenAI Codex, Kimi, OpenRouter, Radius, xAI) | ❌ | |
| Cloudflare auth · Google Vertex ADC · Bedrock ambient creds | ❌ | |
| Multi-provider env-key map | ❌ | only `ANTHROPIC_API_KEY` wired |

## Harness subsystems

| subsystem | status | notes |
|---|---|---|
| System prompt assembly (cwd, tool docs, overrides, `AGENTS.md`/`CLAUDE.md` project context) | ✅ | active-tool prompt, global/project `SYSTEM.md` and `APPEND_SYSTEM.md`, root-to-cwd context discovery, CLI overrides, and transactional reload; like current pi, omits the date for prompt-cache stability |
| Prompt templates (`$1`, `$@`, `$ARGUMENTS`) | ❌ | |
| Sessions / persistence (JSONL/SQLite, resume, fork/clone, branch, tree, import/export, naming) | 🟡 | append-only JSONL, exact global-ID resume, rollback-safe cross-project cwd/session switching with trust re-resolution, tree checkout, forks, branch labels, session-global names, rich shared metadata with incomplete-usage markers, unreadable-file reporting, and operation recovery; no import/export or clone |
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
| TUI selectors/dialogs (model / session / theme / settings / login / oauth / extension / thinking / trust / diff / image / first-run) | 🟡 | generic list/text modals power login, model, rich current/all-project session browsing and naming, tree, recovery, and project-trust flows |
| Rich editor/media features | 🟡 | bracketed paste, history, selection, and mouse are present; no path/file autocomplete, inline image rendering/paste, or external editor |
| Slash commands | 🟡 | 15 built-ins, including `/name`, `/trust`, and `/reload`, versus pi's 22 plus dynamic extension/skill/template commands |
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
tool implementations. It is not exact event parity: pi streams tool progress
and completion-order end events, while Clank finalizes parallel batches
together.

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
