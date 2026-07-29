# Clank

A tiny, memory-efficient coding-agent harness in C++ — a clean reimplementation
of the core of [`earendil-works/pi`](https://github.com/earendil-works/pi)
without a JavaScript runtime.

The goal is footprint. A Node/Bun-based agent carries a fixed ~100–300 MB+
baseline (embedded JS engine + SDK object graphs) and duplicates the transcript
several times in UTF-16. Clank runs the same agent loop with an
arena-allocated, UTF-8, zero-copy transcript.

**Measured:** a full faux run (libcurl init + agent + a `shell` tool execution)
peaks at **~6 MB RSS** (`CLANK_PRINT_MEM=1`).

## What it is

A runnable Clank agent: the agent loop + the Anthropic Messages provider
(HTTP/SSE) + `read`, `write`, `edit`, `ls`, `find`, `grep`, and `shell` tools +
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
  session/ append-only session records, reducer, memory + JSONL repositories
  agent/   durable AgentHarness, recovery driver, hooks, events, compaction
  tools/   read/write/edit/ls/find/grep/shell, direct/shell process exec,
           ripgrep discovery, fs/glob/diff/truncate/filelock infra
  term/    full-screen TUI: raw-mode terminal, key decoder, differential
           renderer, markdown, transcript model, multi-line editor, controller
  app/     CLI entry + durable session startup + TUI/plain REPL + login flow
tests/     single CLI test harness (suite selection via args; offline by default)
```

## Design notes

- **No C++ standard library**, no exceptions, no RTTI. Types and conventions
  (`Arena`, `Array<T>`, PascalCase fields, `FFI(cond)` fastfail) follow the
  caboose/Briq house style. Only the C runtime and platform/libcurl APIs are used.
- **Append-only durable sessions.** Session entries form a branchable message
  tree; harness entries persist provider/tool intent, queues, cancellation, and
  recovery state without entering model context. Session-global names are
  separate from branch labels, and a shared metadata projection derives titles,
  activity/duration statistics, counts, ancestry, and lifetime attempt usage.
  JSONL is the default backend, while `--no-session` uses the same contracts
  entirely in memory.
- **Single-threaded loop, parallel tool batches.** A pluggable `Provider`
  streams one assistant turn (auto-retrying transient errors with capped
  exponential backoff); a turn's tool calls run **concurrently by default** (each
  on a small worker pool, isolated in its own arena), matching pi. Transient
  completion events are published in actual finish order, while final results
  remain durably appended in original call order. Tools receive centrally
  parsed, schema-validated, coercion-normalized arguments; unsupported
  validation keywords fail harness initialization. The `shell` tool also emits
  bounded, replace-in-place partial output snapshots. These live updates never
  enter model context or the session log, so the source-ordered durable result
  remains authoritative for replay and recovery. Set the agent to sequential —
  or mark a tool sequential — to serialize. A `beforeTool` gate and an
  `afterToolCall` hook bracket each call. Events are pushed to an `AgentSink`.
  Mirrors pi's event sequence (`agent_start → turn_start → message_* → tool_* →
  turn_end → agent_end`) plus its agent-owned steer / follow-up / next-turn
  message queues (`queue_update` events).
- **Providers are reimplemented wire protocols**, not vendor SDKs. The Anthropic
  request builder and SSE parser are ~400 lines and unit-tested against a
  recorded fixture (offline).

## Build

Requires CMake (3.20+) and a C++17 compiler. libcurl is vendored automatically
via a checksummed release archive (Schannel on Windows, system TLS elsewhere).
The release archive is cached under `out/fetchcontent/`.

The `grep` tool uses a system `rg` when available. Otherwise it downloads the
current ripgrep release to `~/.clank/bin`; set `CLANK_OFFLINE=1` to disable tool
downloads or `CLANK_RG_PATH` to select an existing binary explicitly.

```sh
cmake -S . -B build -G Ninja
cmake --build build
cmake --build build --target clank_tests
ctest --test-dir build
```

On Windows with the VS toolchain, run the commands from a `vcvars64` shell (or
use the VS-bundled CMake/Ninja). `build.ps1` builds only the `clank` executable
by default, skips redundant configure passes, and accepts `-Test` to build and
run the unit tests.

### Visual Studio 2026

Open the repository folder in Visual Studio 2026. The checked-in `.vsconfig`
declares the Desktop development with C++ and CMake components, and Visual
Studio will discover the `vs2026-x64` configure preset automatically. Select
`clank.exe` as the startup target and choose Debug or Release in the toolbar.
The debugger starts Clank with the repository root as its working directory.

The same presets can be used from a Developer PowerShell:

```powershell
cmake --preset vs2026-x64
cmake --build --preset vs2026-debug

# Optional offline tests:
cmake --build --preset vs2026-debug-tests
ctest --preset vs2026-debug
```

## Run

```sh
# Offline demo (no API key, scripted provider) — exercises loop + tools + TUI:
./build/clank --faux

# Live:
./build/clank --login anthropic     # OAuth (Claude Pro/Max), or set ANTHROPIC_API_KEY
./build/clank                        # full-screen TUI; login can also happen inside it
./build/clank -m claude-sonnet-4-6 -p "summarize README.md"
./build/clank --continue             # most recent session for this project
./build/clank --resume SESSION_ID     # exact ID, including sessions from other projects
./build/clank --no-session            # no session filesystem writes
./build/clank --system-prompt SYSTEM.md -p "review this project"
./build/clank --append-system-prompt "Prefer minimal changes"
./build/clank --no-context-files       # skip AGENTS.md / CLAUDE.md discovery
./build/clank --approve                # trust project prompt files for this run
```

## Project context and system prompts

Clank builds the system prompt from the active tool catalog, project
instructions, and cwd. It loads the first regular file named `AGENTS.md`,
`AGENTS.MD`, `CLAUDE.md`, or `CLAUDE.MD` from `~/.clank` and each directory
from the filesystem root through cwd. Global context is applied first, followed
by broad-to-specific project context. Use `--no-context-files` (`-nc`) to
disable this discovery.

`~/.clank/SYSTEM.md` replaces the built-in prompt globally, and
`~/.clank/APPEND_SYSTEM.md` appends instructions. Trusted projects can override
those files with `<cwd>/.clank/SYSTEM.md` and
`<cwd>/.clank/APPEND_SYSTEM.md`. `--system-prompt` and repeatable
`--append-system-prompt` values have highest precedence; each value is read as
a file when it names an existing regular file, otherwise it is treated as
literal text. Custom prompts still receive project context and cwd.

Project prompt overrides require trust. Decisions are stored hierarchically in
`~/.clank/trust.json`; the nearest saved ancestor wins. Set
`defaultProjectTrust` in `~/.clank/settings.json` to `ask` (default), `always`,
or `never`. `--approve` (`-a`) and `--no-approve` (`-na`) override trust for one
run. `/trust` saves a decision for the current or parent folder and requires a
restart; `/reload` reloads settings and prompt/context files while preserving
the current process's resolved trust. If gated project prompt files first
appear after startup, `/reload` fails closed and requires a restart unless an
explicit CLI trust override covers the run.

## TUI

Running interactively on a real terminal launches a full-screen TUI: an
alternate-screen, double-buffered **differential renderer** (own cell grid +
scrollback), markdown-rendered assistant output, a **sticky working-directory +
git-branch line** just above the prompt, a minimal status line along the bottom
(spinner and state on the left; model and token usage right-aligned), and a
**multi-line input editor**. The first frame is painted synchronously before
HTTP initialization, cwd/git context discovery, and credential warmup; key input
starts before the remaining post-frame work. It runs three threads — the agent
loop, a key-input thread, and a render thread (~20fps) so the spinner stays live
while a turn waits on the network. Bursty assistant deltas are revealed
continuously across ~60fps frames at a capped UTF-8 scalar rate instead of
painting a large received chunk all at once. Visual message/tool transitions
wait behind queued text without blocking the agent or tool execution.

The interface is assembled with the internal, arena-backed `clank_ui`
component library. Layout, focus, hit-testing, pointer capture, reusable
widgets, and custom component integration are documented in
[`docs/UI.md`](docs/UI.md).

Keys: **Enter** sends · **Ctrl+J** / Alt+Enter inserts a newline · arrows move
(↑/↓ across lines, then through history at the edges) · **Ctrl+←/→** word jump ·
Home/End · **Shift** + movement selects composer text ·
**Ctrl+W/U/K** delete word / to start / to end · **mouse wheel** scrolls the
transcript · **left-drag** selects transcript or composer text (and auto-scrolls
the transcript) · an overflow-only scrollbar can be clicked or dragged (track
clicks center its thumb) · type during a turn to **steer** · **Ctrl+C** copies
selections via OSC 52; otherwise it interrupts a turn, cancels a command
operation, clears the composer, or quits · double-press **Escape** within 500 ms
to interrupt an active turn · hover or **left click** a turn's first row to
highlight or collapse/expand it · **Ctrl+O** expands all tools when any is
collapsed, otherwise collapses all · **Ctrl+D** quits · **Ctrl+L** repaints.
Honors `NO_COLOR`. Bracketed paste inserts literal multi-line text and replaces
a composer selection.

When stdin/stdout isn't a TTY (pipes, redirected) or with `-p`, it falls back to
a plain line renderer. Plain and piped output write provider deltas immediately
without display pacing. Session commands remain available; interactive
selectors such as `/login` and `/model` require a TTY.

Interactive commands:

- `/login` — choose an OAuth-capable provider, authenticate, then choose a model.
- `/model` — choose a provider and a live-discovered, fallback, or manual model ID.
- `/session` — inspect the current title, IDs, project, status, times, model,
  counts, token categories, and fork ancestry. Token totals use a trailing `+`
  when interrupted attempts have unavailable usage.
- `/name [title]` — set the current session name; an empty TUI value or
  `/name --clear` restores the first-user-message title.
- `/sessions [all]` — browse rich current-project metadata, or all projects.
- `/resume [id]` — choose an available session, including an all-project page,
  or switch directly by exact full ID.
- `/tree` and `/checkout <entry-id>` — inspect or navigate conversation branches.
- `/fork [entry-id]` — fork and switch to a new session.
- `/compact` — durably summarize the active branch.
- `/recover ...` — resume, abort, or reconcile an interrupted operation.
- `/trust` — save project trust for a future Clank process.
- `/reload` — transactionally reload settings and prompt context while idle.
- `/clear` — create a fresh session while preserving provider/model selection.
- `/exit` (or `/quit`) — close Clank and return to the shell.

In the TUI, typing `/` opens a prefix-filtered command flyout over the content
directly above the composer. Provider, model, and manual-entry choices drill down
as pages in the same surface. Use Up/Down to select, Enter to continue or run the
command, Backspace on an empty page to go back, or Escape to dismiss the current
choice.

Commands entered during a TUI turn run afterward; ordinary text still steers the
active turn. In `--faux` mode, session commands work while `/login` and `/model`
are unavailable.

The selected provider/model and `defaultProjectTrust` policy are written
atomically to `~/.clank/settings.json`.
New sessions use built-in defaults, then settings, then the runtime-only `-m`
override. Resumed sessions restore their durable provider/model ancestry; `-m`
adds a new durable model selection. Direct IDs are exact opaque durable IDs.
Cross-project resume enters the session's stored working directory, resolves
project trust again, rebuilds prompt context, and rollback-safely switches the
session, process cwd, and TUI cwd/branch. `--continue` remains scoped to the current project. Sessions
live under `~/.clank/sessions/v1/<project-key>/`; their canonical JSONL files
also store names and per-attempt usage without a sidecar. Unreadable session
files are reported and skipped during browsing/continuation. Interactive
mode may start without credentials and blocks prompts with `/login` guidance;
`-p` remains fail-fast.

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

`clank_tests` is the single test harness for the whole project: one CLI that
lets you (or an agent) pick exactly what to exercise.

```sh
./build/clank_tests                 # every offline suite
./build/clank_tests --list          # suite names, tags, descriptions
./build/clank_tests json session    # only the named suites
./build/clank_tests --filter line   # substring match: linemodel + lineedit
./build/clank_tests --all           # include network-tagged suites
./build/clank_tests --help          # usage
```

Suites tagged `network` (currently `http`, a live TLS/HTTP request that replaces
the old `clank_http_smoke` binary) are skipped by default, so a bare run stays
offline and deterministic. Request one by name or with `--all`; point it at a
local server with `CLANK_TEST_HTTP_URL`. Exit code is 0 on success, 1 on test
failure, 2 on a usage error. `ctest` runs the offline sweep as the `unit` test;
the `network` test is registered but disabled by default.

Adding a suite is a new `tests/test_*.cpp`, a declaration in `tests/tests.h`, and
one row in the `kSuites` table in `tests/test_main.cpp`.

The default run is network-free and key-free: the loop runs against a faux
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
- Automatic compaction thresholds, skills, general extensions, and additional
  providers.
- Fuzzy/regex session search, generated names, pricing, deletion UX, and a
  derived session metadata sidecar.

## License

MIT (matching the upstream pi project this core is modeled on).
