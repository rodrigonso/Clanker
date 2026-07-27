# Feature parity: Clank vs. pi

Tracks how much of the upstream [`earendil-works/pi`](https://github.com/earendil-works/pi)
agent harness Clank currently reimplements.

| | |
|---|---|
| Comparison date | 2026-06-27 |
| pi ref | `f2e9d75` (pi-coding-agent `0.80.2`) |
| Clank ref | `69e8680` |

Clank deliberately reimplements only the **memory-first core**. This
document is the running gap list — it is not a roadmap commitment.

**Legend:** ✅ functional parity · 🟡 partial · ❌ missing

## Architecture mapping

pi is a 5-package TypeScript monorepo. Clank collapses the first four into
its module tree; the orchestrator has no equivalent.

| pi package | role | Clank | status |
|---|---|---|---|
| `ai` | LLM API: 60+ providers, 9 wire protocols, auth, model registry | `src/llm`, `src/net`, `src/auth` | 🟡 |
| `agent` | harness: loop, sessions, compaction, skills, system prompt | `src/agent` | 🟡 |
| `coding-agent` | CLI, 7 tools, modes, slash-commands, extensions, trust | `src/app`, `src/tools`, `src/term` | 🟡 |
| `tui` | differential renderer library | `src/term` | ✅ core |
| `orchestrator` | multi-instance IPC | — | ❌ |

## Core runtime / agent loop

| capability | status | notes |
|---|---|---|
| Event sequence `agent_start → turn_start → message_* → tool_* → turn_end → agent_end` | ✅ | matches pi's order |
| Data model (text / thinking / tool_call / image) | ✅ | `types.h` mirrors `types.ts` |
| Steering · follow-up · `beforeTool` gate | ✅ | agent-owned thread-safe queues drained at pi's points |
| Abort / cancellation | ✅ | shared abort flag (also short-circuits retry backoff) |
| **Parallel tool execution** | ✅ | tool calls in a turn run concurrently (worker pool, per-job arena), results appended in original order; sequential opt-in via agent policy or per-tool `ExecMode` (mirrors Pi) |
| `afterToolCall` hook | ✅ | field-by-field override of result content / isError / terminate in finalize (mirrors Pi) |
| `nextTurn` queue · `queue_update` events | ✅ | agent owns steer/followUp/nextTurn queues + thread-safe push API; `queue_update` carries queue depths |
| Provider retries (`maxRetries` / backoff) | ✅ | retryable-error classifier + capped exponential backoff in the stream loop; default on (maxRetries=3, 2s base, 60s cap), abortable |

## Providers & protocols

| capability | status | notes |
|---|---|---|
| Anthropic Messages provider + faux | ✅ | hand-rolled SSE build/parse, offline unit test |
| ~33 other providers (openai, google, vertex, bedrock, github-copilot, groq, xai, openrouter, …) | ❌ | |
| 8 other wire protocols (openai-completions / responses / codex, google-generative-ai, google-vertex, bedrock-converse, mistral, azure-openai-responses) | ❌ | |
| Prompt caching (`cache_control`) | ❌ | not emitted |
| Thinking / reasoning streaming | 🟡 | Anthropic thinking deltas parsed; no thinking-level control |
| Cross-provider message normalization (`transform-messages`) | ❌ | n/a with a single provider |
| Model registry · `--list-models` · pricing/cost · scoped models | 🟡 | provider registry + live Anthropic `/v1/models` discovery; no pricing or list flag |

## Tools

| pi tool | Clank | notes |
|---|---|---|
| `read` | ✅ | `offset` / `limit`, verbatim text, pi-style head truncation, image content blocks; no image auto-resize |
| `bash` | ✅ | `timeout` in **seconds**, process-tree kill, tail truncation with full-output temp log; no live UI streaming yet |
| `write` | ✅ | creates parent dirs, overwrites, serialized per-file |
| `edit` | ✅ | exact + whitespace-tolerant fuzzy multi-edit, mutation lock, unified diff; NFKC omitted |
| `grep` | ✅ | native regex engine (not `rg`), context / literal / ignore-case, gitignore-lite |
| `find` | ✅ | native glob walk (not `fd`), gitignore-lite |
| `ls` | ✅ | case-insensitive sorted listings, dotfiles included |
| Tool allow/deny lists · read-only bundle | ❌ | |

## Auth

| capability | status | notes |
|---|---|---|
| API key (env + stored) | ✅ | |
| Anthropic OAuth (PKCE + loopback callback) | ✅ | `sk-ant-oat` → Bearer + claude-code beta headers |
| Credential store (`~/.clank/auth.json`, 0600, atomic) · per-request refresh | ✅ | |
| Device-code OAuth (GitHub Copilot, OpenAI Codex) | ❌ | |
| Cloudflare auth · Google Vertex ADC · Bedrock ambient creds | ❌ | |
| Multi-provider env-key map | ❌ | only `ANTHROPIC_API_KEY` wired |

## Harness subsystems

| subsystem | status | notes |
|---|---|---|
| System prompt assembly (date, cwd, tool docs, `AGENTS.md`/`CLAUDE.md` project context) | ❌ | Clank ships a static one-line system prompt |
| Prompt templates (`$1`, `$@`, `$ARGUMENTS`) | ❌ | |
| Sessions / persistence (JSONL, resume, fork, branch, tree, listing, naming) | ❌ | |
| Compaction (auto + manual, branch summarization) | ❌ | |
| Skills (discovery, frontmatter, `<available_skills>` injection, `/skill`) | ❌ | |
| Subagent / task delegation | ❌ | pi also has none at the harness level |

## CLI, modes & TUI

| capability | status | notes |
|---|---|---|
| Print mode (`-p`) · interactive TUI · plain REPL fallback | ✅ | |
| JSON output mode · RPC (JSONL) mode | ❌ | |
| CLI flags | 🟡 | ~7 (`-m -p --login --logout --no-open --faux --help`) vs pi's ~45 |
| TUI core: differential renderer, markdown, multi-line editor, status line, scroll, steering, abort, `NO_COLOR` | ✅ | |
| TUI selectors/dialogs (model / session / theme / settings / login / oauth / extension / thinking / trust / diff / image / first-run) | 🟡 | generic list/text modals power login and model selection |
| Slash commands | 🟡 | `/login`, `/model`, `/clear`, `/exit` (`/quit` alias) |
| Themes · configurable keybindings | ❌ | |

## Platform features

| capability | status | notes |
|---|---|---|
| Extensions (register tools / commands / shortcuts / flags / renderers / providers; UI hooks; MCP) | ❌ | |
| Trust / project-trust · `settings.json` · `keybindings.json` | 🟡 | atomic provider/model settings; no trust or keybindings |
| Selection / clipboard copy | ✅ | transcript and composer; `Ctrl+C` sends OSC 52 |
| Export HTML · import · share-as-gist | ❌ | |
| Orchestrator (multi-instance IPC: serve/spawn/list/stop/rpc) | ❌ | |
| Telemetry / diagnostics | ❌ | |

## Bottom line

At parity on the **memory-first core**: data model, loop semantics, Anthropic
SSE, OAuth, a real full-screen differential TUI, and the seven-tool coding
suite. The open gaps are (a) **breadth** — providers, wire protocols, and CLI
surface — and (b) the **coding-agent layer** — sessions, compaction, skills,
slash-commands, extensions, trust, and rich system-prompt context assembly.

## Refreshing this report

```sh
# clone the current pi and re-survey
git clone --depth 1 https://github.com/earendil-works/pi.git /tmp/pi
# pi tools:      packages/coding-agent/src/core/tools/index.ts
# pi providers:  packages/ai/src/providers/all.ts
# pi protocols:  packages/ai/src/api/compat.ts
# pi loop:       packages/agent/src/agent-loop.ts
# pi cli flags:  packages/coding-agent/src/cli/args.ts
# pi slash cmds: packages/coding-agent/src/core/slash-commands.ts
```

Update the metadata table (date + pi ref) when re-running.
