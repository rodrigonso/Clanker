# Clank vs. jcode

Comparison date: 2026-07-27

- Clank implementation ref: `2af5cd95cd35fde4431d84dfc749642ae8811672`
- jcode ref: `c7f487309ad693c61a6b3268132ff2cb2813fced` on `master`
  (`0.61.0` in `Cargo.toml`); its README benchmarks cite a separate
  `v0.9.1888-dev` build

## Summary

Clank wins on raw footprint, binary size, and architectural simplicity. jcode
wins decisively on product breadth, long-session management, integrations,
safety controls, and multi-agent workflows.

Clank is the stronger fit for a small, embeddable, Anthropic-focused coding
agent. jcode is the stronger fit for a comprehensive coding-agent product.

## Performance

| Metric | Clank | jcode |
|---|---:|---:|
| Idle memory | 8.5 MiB working set measured on Windows | 27.8 MB PSS with embeddings off; 167.1 MB by default, author-reported on Linux |
| Local startup measurement | 56 ms average for process startup, a faux agent turn, and a shell command | 14.0 ms to first frame; 48.7 ms to first input, author-reported |
| Binary size | 689 KiB | Not documented |
| Tool concurrency | Automatic parallel batches, maximum 8 workers | Explicit `batch` tool, maximum 10 calls; background jobs and swarm |
| Multi-session model | Independent process with append-only JSONL state per session | Shared daemon amortizes state; claimed approximately 9.9 MB per additional session |
| Long-session behavior | Durable manual compaction, branching, resume, and explicit crash recovery | Background compaction, persistence, and crash recovery |

These measurements are directional rather than directly comparable. They use
different operating systems and memory metrics: Clank was measured using
Windows working set/RSS, while jcode reports Linux PSS from its own benchmark.
No independent third-party jcode benchmark was found.

Clank's faux run peaked at approximately 8.8 MiB RSS. Its arenas lazily reserve
about 6.2 GiB of virtual address space in the initialized process, but this is
not committed physical memory.

### Performance characteristics

- Clank avoids a language runtime, uses zero-copy UTF-8 transcript slices, and
  commits arena pages lazily.
- Clank automatically executes independent tool calls concurrently and records
  their results in the original call order.
- jcode uses a shared server process, asynchronous Tokio execution, background
  commands, MCP connection pooling, and swarm workers.
- jcode's ripgrep-backed `agentgrep` and adaptive result truncation should scale
  better on large repositories than Clank's serial native grep, which reads
  each searched file fully.
- Clank starts with a substantially smaller resident footprint. Durable history
  lives in JSONL, per-operation working arenas are reset, and manual compaction
  bounds model context; jcode adds automatic and semantic policies.
- jcode's default local embedding model accounts for much of the difference
  between its 27.8 MB embeddings-off and 167.1 MB default memory results.

## Feature comparison

| Area | Clank | jcode |
|---|---|---|
| Providers | Anthropic and faux provider | Claude, OpenAI, Gemini, Copilot, Azure, OpenRouter, Bedrock, and many OpenAI-compatible providers |
| Authentication | Anthropic API keys and OAuth with automatic refresh | Multiple OAuth providers, API keys, custom endpoints, and multi-account switching |
| Coding tools | `read`, `write`, `edit`, `ls`, `find`, `grep`, `shell` | 30+ tools including patching, browser/web access, background jobs, memory, skills, MCP, todo, session search, and swarm |
| Editing | Exact and whitespace-tolerant multi-edit with unified diff | Edit, multiedit, unified patch, and Codex-compatible `apply_patch` |
| Code intelligence | Native grep/find | Ripgrep-backed grep/find plus outline, trace, smart search, and context-aware result truncation |
| LSP/tree-sitter | None | None found |
| Sessions | Append-only JSONL sessions with continue/resume, branches, tree checkout, forks, and explicit recovery | Persistent server sessions, resume/import, journals, and crash recovery |
| Context management | Durable manual branch compaction | Reactive, proactive, and semantic compaction |
| Memory | Transcript only | Persistent long-term memory, embeddings, retrieval, and prior-session search |
| Subagents | None | Worktree-aware swarm, recursive workers, messaging, and file-conflict notifications |
| MCP | None | Stdio MCP servers with shared connections and Claude-compatible configuration imports |
| IDE/clients | TUI and plain/print modes | TUI, ACP editor bridge, desktop clients, and a persistent client/server model |
| Shell handling | Timeout/abort kills the full process tree | Async commands, background handoff on foreground timeout, and a dedicated background-task tool |
| Safety | Optional host `BeforeTool` hook, but no built-in approval UI or sandbox | Implemented destructive-command classifier, reflection gate, and absolute catastrophic-path denial; no OS-level sandbox. The broader human-review system is design-stage |
| Extensibility | Compile-time provider/tool seams plus awaited typed hooks and watch events | MCP, hooks, skills, custom providers, self-development, rebuild, and hot reload |
| Observability | Last-turn token counts and optional peak-RSS output | Token costs, provider usage, pricing catalog, cache diagnostics, timing logs, and opt-out telemetry |
| Platform support | Windows and POSIX C++ implementations | Linux, macOS, Windows, FreeBSD smoke testing, and Termux support |

## Prompt caching

Both harnesses support prompt-cache-aware behavior. Clank emits Anthropic
`cache_control` breakpoints for the system prompt or tool prefix and the most
recent message groups:

- `src/llm/anthropic_proto.cpp:141-145`
- `src/llm/anthropic_proto.cpp:219-265`

jcode additionally sorts tool definitions deterministically, hashes the
cache-relevant message projection, reports cache misses, and tracks cache-read
and cache-write pricing.

## Maturity and complexity

Clank is a deliberately small codebase with 22 commits, no tags, and a narrow
scope. Its design is easy to audit and has few dependencies, but major
coding-agent subsystems remain intentionally absent.

jcode is a much larger, pre-1.0 Rust workspace with 81 workspace members, broad
CI, frequent releases, and more than 12,000 GitHub stars. That breadth brings
greater maintenance and operational complexity. Its documentation also mixes
shipped features with explicitly design-stage systems; the broader human-review
safety system should not be treated as fully implemented.

## Recommendation

Retain Clank when the priority is:

- minimal resident memory and binary size;
- a small, auditable native core;
- Anthropic-focused usage;
- automatic low-overhead parallel tool execution; or
- embedding the harness into a larger native application.

Adopt or emulate jcode capabilities when the priority is:

- broad provider and authentication support;
- automatic/semantic compaction and long-term memory;
- MCP, skills, browser tools, and custom integrations;
- large-repository search;
- command safety controls; or
- coordinated multi-agent development.

## Sources

### Clank

- `README.md`
- `docs/PARITY.md`
- `CMakeLists.txt`
- `src/agent/agent.cpp`
- `src/agent/agent.h`
- `src/app/main.cpp`
- `src/llm/anthropic_proto.cpp`
- `src/tools/grep.cpp`

### jcode

- <https://github.com/1jehuang/jcode>
- <https://github.com/1jehuang/jcode/blob/c7f487309ad693c61a6b3268132ff2cb2813fced/Cargo.toml>
- <https://github.com/1jehuang/jcode/blob/c7f487309ad693c61a6b3268132ff2cb2813fced/crates/jcode-app-core/src/tool/mod.rs>
- <https://github.com/1jehuang/jcode/blob/c7f487309ad693c61a6b3268132ff2cb2813fced/crates/jcode-app-core/src/tool/batch.rs>
- <https://github.com/1jehuang/jcode/blob/c7f487309ad693c61a6b3268132ff2cb2813fced/crates/jcode-command-risk/src/lib.rs>
- <https://github.com/1jehuang/jcode/blob/c7f487309ad693c61a6b3268132ff2cb2813fced/docs/SERVER_ARCHITECTURE.md>
- <https://github.com/1jehuang/jcode/blob/c7f487309ad693c61a6b3268132ff2cb2813fced/docs/SWARM_ARCHITECTURE.md>
- <https://github.com/1jehuang/jcode/blob/c7f487309ad693c61a6b3268132ff2cb2813fced/docs/MEMORY_ARCHITECTURE.md>
- <https://github.com/1jehuang/jcode/blob/c7f487309ad693c61a6b3268132ff2cb2813fced/docs/SAFETY_SYSTEM.md>
