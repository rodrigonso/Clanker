# mini-agent

A tiny, memory-efficient coding-agent harness in C++ — a clean reimplementation
of the core of [`earendil-works/pi`](https://github.com/earendil-works/pi)
without a JavaScript runtime.

The goal is footprint. A Node/Bun-based agent carries a fixed ~100–300 MB+
baseline (embedded JS engine + SDK object graphs) and duplicates the transcript
several times in UTF-16. mini-agent runs the same agent loop with an
arena-allocated, UTF-8, zero-copy transcript.

**Measured:** a full faux run (libcurl init + agent + a `bash` tool execution)
peaks at **~6 MB RSS** (`MA_PRINT_MEM=1`).

## What it is

A runnable mini-agent: the agent loop + the Anthropic Messages provider
(HTTP/SSE) + `read` and `bash` tools + a streaming REPL.

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
  tools/   read, bash, cross-platform process exec
  app/     CLI entry + REPL renderer + --login flow
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
- **Synchronous, single-threaded loop.** A pluggable `Provider` streams one
  assistant turn; tools execute in order; events are pushed to an `AgentSink`.
  Mirrors pi's event sequence (`agent_start → turn_start → message_* → tool_* →
  turn_end → agent_end`) and keeps its steering/follow-up seams.
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
# Offline demo (no API key, scripted provider) — exercises loop + tools + REPL:
./build/mini-agent --faux

# Live:
./build/mini-agent --login anthropic     # OAuth (Claude Pro/Max), or set ANTHROPIC_API_KEY
./build/mini-agent                        # interactive REPL
./build/mini-agent -m claude-sonnet-4-6 -p "summarize README.md"
```

Interactive: Ctrl+C interrupts the current turn, `/exit` quits.
Set `MA_PRINT_MEM=1` to print peak RSS at exit.

## Authentication

Two credential types, stored in `~/.mini-agent/auth.json` (atomic writes,
restricted perms), resolved per turn so OAuth tokens refresh mid-session:

- **API key** — set `ANTHROPIC_API_KEY`, or store one in `auth.json`.
- **OAuth (subscription)** — `mini-agent --login anthropic` runs an
  authorization-code + PKCE flow: it opens your browser to approve access and a
  local callback server on `http://localhost:53692/callback` captures the
  redirect automatically (no copy/paste). The browser shows a success page; the
  access/refresh tokens are saved and auto-refreshed. Pass `--no-open` to print
  the URL instead of launching a browser. `--logout <provider>` removes them.

The OAuth flow/storage is **provider-generic** (`src/auth/`): a config table
(`OAuthConfig`) plus generic PKCE/exchange/refresh. Only how the token is applied
to a request is per-provider — for Anthropic, an `sk-ant-oat` token switches to
`Authorization: Bearer` with the Claude Code identity headers and system block.
Adding Copilot/OpenAI is one config row plus a small header branch.

Resolution priority: stored api_key → stored OAuth (refresh) → env var.

## Test

```sh
./build/ma_tests          # unit tests (base, json, loop, anthropic, tools, crypto, auth, loopback)
./build/ma_http_smoke     # TLS/HTTP smoke against https://example.com
```

The unit suite is network-free and key-free: the loop runs against a faux
provider, the Anthropic SSE parser against a recorded fixture, the OAuth module
against fixtures (PKCE vectors, URL/body building, response parsing, credential
round-trips), and the callback server against an in-process loopback socket
(request parse, code/state capture, state-mismatch rejection).

## Scope / deferred

This is the small clean core. Deliberately out of scope for now (the structure
leaves seams for them):

- Additional providers (OpenAI-compatible shape). OAuth is generic; each new
  provider is a config row + a small token-application branch.
- Device-code OAuth (e.g. GitHub Copilot) — the loopback callback flow is done.
- Raw-mode terminal line editing and a steering input thread (the REPL uses
  simple line input + signal-based abort; the loop already supports steering
  via its poll seams).
- Parallel tool execution, compaction, on-disk sessions, skills, slash commands,
  and extensions.

## License

MIT (matching the upstream pi project this core is modeled on).
