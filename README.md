# Clank

A tiny, memory-efficient coding-agent harness in C++ — a clean reimplementation
of the core of [`earendil-works/pi`](https://github.com/earendil-works/pi)
without a JavaScript runtime.

The goal is footprint. A Node/Bun-based agent carries a fixed ~100–300 MB+
baseline (embedded JS engine + SDK object graphs) and duplicates the transcript
several times in UTF-16. Clank runs the same agent loop with an
arena-allocated, UTF-8, zero-copy transcript.

**Measured:** a full faux run (libcurl init + agent + a `bash` tool execution)
peaks at **~6 MB RSS** (`CLANK_PRINT_MEM=1`).

## What it is

A runnable Clank agent: the agent loop + the Anthropic Messages provider
(HTTP/SSE) + `read`, `write`, `edit`, `ls`, `find`, `grep`, and `bash` tools +
a streaming REPL.

```
src/
  base/    Arena (cross-platform reserve/commit), Array<T>, UTF-8 Str/StrBuf,
           FFI fastfail, peak-RSS reporter   (caboose conventions, no C++ std)
  json/    arena DOM parser + escaping writer (hand-rolled, no deps)
  crypto/  SHA-256, base64url, secure random, PKCE (no deps)
  llm/     data model, faux provider, Anthropic protocol (build + SSE parse)
  net/     streaming HTTP + non-streaming fetch over libcurl; loopback OAuth
           callback server + browser opener (no deps beyond OS sockets)
  auth/    credentials + generic OAuth (PKCE flow, callback capture, store,
           refresh, resolve)
  agent/   the synchronous turn/tool/steering loop + Agent state
  tools/   read/write/edit/ls/find/grep/bash, process exec, fs/glob/regex/
           diff/truncate/filelock infra
  term/    full-screen TUI: raw-mode terminal, key decoder, differential
           renderer, markdown, transcript model, multi-line editor, controller
  app/     CLI entry + TUI/plain REPL + --login flow
tests/     offline unit tests (no network, no API key)
```

## Design notes

- **No C++ standard library**, no exceptions, no RTTI. Types and conventions
  (`Arena`, `Array<T>`, PascalCase fields, `FFI(cond)` fastfail) follow the
  caboose/Briq house style. Only the C runtime and platform/libcurl APIs are used.
- **Memory-first transcript.** Messages are flat structs; all text is a
  `StrView` (non-owning UTF-8 slice) into the arena. There is no per-turn
  `convertToLlm` duplication (identity conversion) and streaming deltas are
  forwarded as zero-copy slices rather than whole-message clones.
- **Single-threaded loop, parallel tool batches.** A pluggable `Provider`
  streams one assistant turn (auto-retrying transient errors with capped
  exponential backoff); a turn's tool calls run **concurrently by default** (each
  on a small worker pool, isolated in its own arena), matching pi, with results
  appended to the transcript in original call order; set the agent to sequential
  — or mark a tool sequential — to serialize. A `beforeTool` gate and an
  `afterToolCall` hook bracket each call. Events are pushed to an `AgentSink`.
  Mirrors pi's event sequence (`agent_start → turn_start → message_* → tool_* →
  turn_end → agent_end`) plus its agent-owned steer / follow-up / next-turn
  message queues (`queue_update` events).
- **Providers are reimplemented wire protocols**, not vendor SDKs. The Anthropic
  request builder and SSE parser are ~400 lines and unit-tested against a
  recorded fixture (offline).

## Build

Requires CMake (3.20+) and a C++17 compiler. libcurl is vendored automatically
via `FetchContent` (Schannel on Windows, system TLS elsewhere).

```sh
cmake -S . -B build -G Ninja
cmake --build build
```

On Windows with the VS toolchain, run the commands from a `vcvars64` shell (or
use the VS-bundled CMake/Ninja).

## Run

```sh
# Offline demo (no API key, scripted provider) — exercises loop + tools + TUI:
./build/clank --faux

# Live:
./build/clank --login anthropic     # OAuth (Claude Pro/Max), or set ANTHROPIC_API_KEY
./build/clank                        # full-screen TUI; login can also happen inside it
./build/clank -m claude-sonnet-4-6 -p "summarize README.md"
```

## TUI

Running interactively on a real terminal launches a full-screen TUI: an
alternate-screen, double-buffered **differential renderer** (own cell grid +
scrollback), markdown-rendered assistant output, a **sticky working-directory +
git-branch line** just above the prompt, a minimal status line along the bottom
(spinner and state on the left; model and token usage right-aligned), and a
**multi-line input editor**. It runs three threads — the agent loop, a key-input
thread, and a render thread (~20fps) so the spinner stays live while a turn waits
on the network. Bursty assistant deltas are revealed continuously across ~60fps
frames at a capped UTF-8 scalar rate instead of painting a large received chunk
all at once. Visual message/tool transitions wait behind queued text without
blocking the agent or tool execution.

Keys: **Enter** sends · **Ctrl+J** / Alt+Enter inserts a newline · arrows move
(↑/↓ across lines, then through history at the edges) · **Ctrl+←/→** word jump ·
Home/End · **Ctrl+W/U/K** delete word / to start / to end · **mouse wheel**
scrolls the transcript · type during a turn to **steer** · **Ctrl+C** interrupts a
turn, cancels a command operation, or quits when idle · hover or **left click**
the first row of a transcript turn to highlight or collapse/expand it ·
**Ctrl+O** expands all tools when any
is collapsed, otherwise collapses all · **Ctrl+D** quits · **Ctrl+L** repaints.
Honors `NO_COLOR`. Pasted multi-line text is inserted literally (bracketed paste).

When stdin/stdout isn't a TTY (pipes, redirected) or with `-p`, it falls back to
a plain line renderer. Plain and piped output write provider deltas immediately
without display pacing. Piped sessions support `/clear` and `/exit`; interactive
selectors such as `/login` and `/model` require a TTY.

Interactive commands:

- `/login` — choose an OAuth-capable provider, authenticate, then choose a model.
- `/model` — choose a provider and a live-discovered, fallback, or manual model ID.
- `/clear` — clear the conversation while preserving provider, model, and credentials.
- `/exit` (or `/quit`) — close Clank and return to the shell.

In the TUI, typing `/` opens a prefix-filtered command flyout over the content
directly above the composer. Provider, model, and manual-entry choices drill down
as pages in the same surface. Use Up/Down to select, Enter to continue or run the
command, Backspace on an empty page to go back, or Escape to dismiss the current
choice.

Commands entered during a TUI turn run afterward; ordinary text still steers the
active turn. In `--faux` mode, `/clear` and `/exit` work while `/login` and
`/model` are unavailable.

The selected provider/model is written atomically to `~/.clank/settings.json`.
Startup precedence is built-in defaults, then settings, then the runtime-only
`-m` override. Interactive mode may start without credentials and blocks prompts
with `/login` guidance; `-p` remains fail-fast.

Set `CLANK_PRINT_MEM=1` to print peak RSS at exit.

## Authentication

Two credential types, stored in `~/.clank/auth.json` (atomic writes,
restricted perms), resolved per turn so OAuth tokens refresh mid-session.
Existing `~/.mini-agent/auth.json` credentials are migrated automatically:

- **API key** — set `ANTHROPIC_API_KEY`, or store one in `auth.json`.
- **OAuth (subscription)** — `clank --login anthropic` runs an
  authorization-code + PKCE flow: it opens your browser to approve access and a
  local callback server on `http://localhost:53692/callback` captures the
  redirect automatically (no copy/paste). The browser shows a success page; the
  access/refresh tokens are saved and auto-refreshed. Pass `--no-open` to print
  the URL instead of launching a browser. Interactive `/login` uses the same
  flow. `--logout <provider>` removes credentials.

The OAuth flow/storage is **provider-generic** (`src/auth/`): a config table
(`OAuthConfig`) plus generic PKCE/exchange/refresh. Only how the token is applied
to a request is per-provider — for Anthropic, an `sk-ant-oat` token switches to
`Authorization: Bearer` with the Claude Code identity headers and system block.
Adding Copilot/OpenAI is one config row plus a small header branch.

Resolution priority: stored api_key → stored OAuth (refresh) → env var.

## Test

```sh
./build/clank_tests       # unit tests: base, json, loop, anthropic, tools, crypto,
                          #   auth, loopback, threads, keys, screen, markdown,
                          #   linemodel, lineedit, tui, commands, settings,
                          #   provider registry/model discovery
./build/clank_http_smoke  # TLS/HTTP smoke against https://example.com
```

The unit suite is network-free and key-free: the loop runs against a faux
provider, the Anthropic SSE parser against a recorded fixture, the OAuth module
against fixtures, the callback server against an in-process loopback socket, and
the TUI via pure units (key decoder, differential-renderer diff, markdown,
line-wrapping, the multi-line editor) plus a headless controller test driving the
event→transcript mapping. Live full-screen rendering is verified manually.

## Scope / deferred

This is the small clean core. Deliberately out of scope for now (the structure
leaves seams for them):

- Additional providers (OpenAI-compatible shape). OAuth is generic; each new
  provider is a config row + a small token-application branch.
- Device-code OAuth (e.g. GitHub Copilot) — the loopback callback flow is done.
- TUI niceties: visual-row (vs logical-line) Up/Down, syntax highlighting,
  images, undo. The full-screen differential TUI itself is done.
- Compaction, on-disk sessions, skills, additional slash commands, and extensions.

## License

MIT (matching the upstream pi project this core is modeled on).
