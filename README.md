# Clank

A tiny, memory-efficient coding-agent harness in C++ — a clean reimplementation
of the core of [`earendil-works/pi`](https://github.com/earendil-works/pi)
without a JavaScript runtime.

The goal is footprint. A Node/Bun-based agent pays for an embedded JS engine and
SDK object graphs before it does any work, and duplicates the transcript several
times in UTF-16. Clank runs the same agent loop with an arena-allocated, UTF-8,
zero-copy transcript.

**Measured** on Windows x64 with `CLANK_PRINT_MEM=1`: a complete live run
(libcurl + TLS + one prompt and response over HTTPS) peaks at **~12.4 MB RSS**
from a Release build. For scale, an empty `node -e ''` process
on the same machine (Node 24.13.0) sits at ~33 MB before loading an agent at
all. The claim here is the order of magnitude, not a benchmarked ratio against
a specific upstream measurement — pi publishes no memory numbers.

## What it is

A runnable Clank agent: the agent loop + native Anthropic Messages and OpenAI
Responses providers (HTTP/SSE) + `read`, `write`, `edit`, `ls`, `find`, `grep`,
`shell`, and `ask_user` tools + a streaming REPL.

```
src/
  base/    Arena (cross-platform reserve/commit), Array<T>, UTF-8 Str/StrBuf,
           FFI fastfail, peak-RSS reporter   (caboose conventions, no C++ std)
  json/    arena DOM parser + escaping writer (hand-rolled, no deps)
  crypto/  SHA-256, base64url, secure random, PKCE (no deps)
  llm/     data model, Anthropic Messages and OpenAI Responses
           protocols (request build + SSE parse), retry policy, model registry
  net/     streaming HTTP + non-streaming fetch over libcurl; loopback OAuth
           callback server + browser opener (no deps beyond OS sockets)
  auth/    credentials + generic OAuth (PKCE flow, callback capture, store,
           refresh, resolve)
  session/ append-only v2 message tree, reducer, memory + JSONL repositories
  agent/   incremental AgentHarness, in-memory queues, hooks, events, compaction
  tools/   read/write/edit/ls/find/grep/shell/ask_user, direct/shell process
           exec, ripgrep discovery, fs/glob/diff/truncate/filelock infra
  term/    full-screen TUI: raw-mode terminal, key decoder, differential
           renderer, markdown, transcript model, multi-line editor, controller
  app/     CLI entry + durable session startup + TUI/plain REPL + login flow +
           external lifecycle hooks (hooks.json config + dispatch runner)
tests/     single CLI test harness (suite selection via args; offline by default)
```

## Design notes

- **No C++ standard library**, no exceptions, no RTTI. Types and conventions
  (`Arena`, `Array<T>`, PascalCase fields, `FFI(cond)` fastfail) follow the
  caboose/Briq house style. Only the C runtime and platform/libcurl APIs are used.
- **Append-only durable sessions.** Session v2 entries form a branchable,
  message-oriented tree containing only model-visible state, compactions,
  names, labels, and the selected leaf. Active turns and message queues remain
  in memory, matching pi. A crash can leave an assistant tool call without a
  result; provider replay supplies a transient `No result provided` error
  without persisting it or rerunning the side effect. JSONL is the default
  backend, while `--no-session` uses the same contracts entirely in memory.
- **Single-threaded loop, parallel tool batches.** A pluggable `Provider`
  streams one assistant turn; a turn's tool calls run **concurrently by default** (each
  on a small worker pool, isolated in its own arena), matching pi. Transient
  completion events are published in actual finish order, while final results
  are appended in original call order. Provider-request and whole-assistant-turn
  retry layers are separately configurable and default to off. Tools receive centrally
  parsed, schema-validated, coercion-normalized arguments; unsupported
  validation keywords fail harness initialization. The `shell` tool also emits
  bounded, replace-in-place partial output snapshots. These live updates never
  enter model context or the session log, so the source-ordered durable result
  remains authoritative for replay. Set the agent to sequential —
  or mark a tool sequential — to serialize. A `beforeTool` gate and an
  `afterToolCall` hook bracket each call. Events are pushed to an `AgentSink`.
  Mirrors pi's event sequence (`agent_start → turn_start → message_* → tool_* →
  turn_end → agent_end`) plus its agent-owned steer / follow-up / next-turn
  message queues (`queue_update` events).
- **Full hook parity with pi's `AgentLoopConfig`.** Thirteen typed C callbacks
  bracket the loop: `BeforeRun`, `BeforeContext`, `ConvertToLlm`,
  `BeforeProvider`, `AfterProvider`, `BeforeTool`, `PrepareNextTurn`,
  `ShouldStopAfterTurn`, `GetSteeringMessages`, `GetFollowUpMessages`,
  `BeforeCompaction`, `BeforeNavigation`, `AfterTurn`. `PrepareNextTurn` swaps
  the model, effort, or context for the rest of a run and `ShouldStopAfterTurn`
  ends it gracefully; both fire after `turn_end` and before either steering
  source is polled. `GetSteeringMessages` / `GetFollowUpMessages` are polled at
  the same checkpoints as the in-memory queues. Overrides are run-scoped and
  never touch the config or the session tree; a failing hook surfaces as a
  typed `HookError`.
- **Providers are reimplemented wire protocols**, not vendor SDKs. Anthropic
  Messages and OpenAI Responses request builders and SSE parsers are tested
  against recorded fixtures offline.
- **External lifecycle hooks** let an editor or supervisor follow a run without
  linking against Clank. See [Lifecycle hooks](#lifecycle-hooks).

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
run the unit tests. `run.ps1` builds incrementally and then launches the
executable in your current directory:

```powershell
.\run.ps1                             # build, then start the TUI
.\run.ps1 -- -c                       # forward Clank's own flags after --
.\run.ps1 -NoBuild --help             # skip the build, run what's there
```

Put `--` before Clank's short options: without it PowerShell binds `-c` to
`run.ps1`'s own `-Config`.

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
./build/clank --login anthropic     # OAuth (Claude Pro/Max), or set ANTHROPIC_API_KEY
./build/clank --login openai        # OAuth (ChatGPT subscription), or set OPENAI_API_KEY
./build/clank                        # full-screen TUI; login can also happen inside it
./build/clank -m claude-sonnet-4-6 -p "summarize README.md"
./build/clank --mode json -p "summarize README.md"   # NDJSON event stream
./build/clank --continue             # most recent session for this project
./build/clank --resume SESSION_ID     # exact ID, including sessions from other projects
./build/clank --no-session            # no session filesystem writes
./build/clank --system-prompt SYSTEM.md -p "review this project"
./build/clank --append-system-prompt "Prefer minimal changes"
./build/clank --no-context-files       # skip AGENTS.md / CLAUDE.md discovery
./build/clank --no-skills              # skip SKILL.md discovery
./build/clank --skill ./my-skill       # load one skill explicitly (repeatable)
./build/clank --no-prompt-templates    # skip prompt-template discovery
./build/clank --prompt-template ./review.md   # load one template (repeatable)
./build/clank -t read,ls,grep          # allowlist of tools to enable
./build/clank -xt shell                # disable specific tools
./build/clank --no-tools               # read-only conversation
./build/clank --max-turns 25           # ask to continue every 25 turns (0: unlimited)
./build/clank --approve                # trust project prompt files for this run
```

## Project context and system prompts

Clank builds the system prompt from the active tool catalog, project
instructions, and cwd. It loads the first regular file named
`AGENTS.override.md`, `AGENTS.md`, `AGENTS.MD`, `CLAUDE.md`, or `CLAUDE.MD`
from `~/.clank` and each directory from the filesystem root through cwd.
`AGENTS.override.md` therefore wins over the other candidates in its own
directory, while every other directory still layers normally. Global context is
applied first, followed by broad-to-specific project context. Use
`--no-context-files` (`-nc`) to disable this discovery.

Tools contribute their own prompt text: a one-line snippet for the `Available
tools` list and optional guideline bullets that are merged, in tool order, with
the always-on guidelines. A tool without a snippet is not listed.

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

`~/.clank/settings.json` also stores the active provider/model and explicit
effort or selectable context-window preferences. Unsupported saved values fall
back to the selected model's defaults. Current fixed provider context windows are displayed without an extra chooser.

## Skills

Clank implements the [Agent Skills](https://agentskills.io/specification)
standard. A skill is a `SKILL.md` file with YAML frontmatter carrying a `name`
and a required `description`; everything after the frontmatter is the body.

```markdown
---
name: release-notes
description: Draft release notes from the commits since the last tag.
---

Read `CHANGELOG.md`, then summarize each merged change in one line.
```

Only the name, description, and file location enter the system prompt, as an
`<available_skills>` block placed after the project context. The body is loaded
on demand — either the model calls `read` when a task matches the description,
or the user runs `/skill:<name>`. The block is emitted only when the `read`
tool is active, since nothing could load the bodies otherwise. A skill whose
frontmatter sets `disable-model-invocation: true` is hidden from the prompt and
reachable only through `/skill:`.

Skills are discovered from, in order:

1. `~/.clank/skills/`
2. `~/.agents/skills/`
3. `.agents/skills/` in each directory from the repository root through cwd
4. `<cwd>/.clank/skills/`
5. the `skills` array in `~/.clank/settings.json`
6. `--skill <path>`, repeatable

Roots 3 and 4 are project-local and require trust, like project prompt
overrides. `--no-skills` disables roots 1–5; `--skill` still loads, so
`--no-skills --skill ./one` loads exactly one skill.

**The first root to claim a name wins**, so a user-scoped skill shadows a
project skill of the same name rather than the other way round. This is
deliberate parity with pi, and it is the opposite of the usual override
direction — rename the project skill if you want both.

Within a root, a directory containing `SKILL.md` is one skill and is not
descended into; its subdirectories are that skill's own resources. Otherwise
subdirectories are searched recursively, skipping dot-directories and
`node_modules` and honoring `.gitignore`, `.ignore`, and `.fdignore`. Bare
`*.md` files count as skills only at the root of a skills directory. Duplicate
paths reached through symlinks are loaded once.

`/skill:<name> [instructions]` submits the skill body as a prompt, wrapped so
relative references resolve against the skill's own directory, with any trailing
instructions appended. Loaded skills appear in the TUI command flyout, and
`/reload` re-runs discovery.

```sh
./build/clank --skill ./tools/review-skill      # load one skill explicitly
./build/clank --no-skills                        # skip all skill discovery
```

## Prompt templates

A prompt template is a `.md` file whose body becomes a slash command. Unlike
skills, templates never enter the system prompt — the model does not know they
exist. They are shorthand for the human at the keyboard.

```markdown
---
description: Review a file for correctness.
argument-hint: <path> [focus]
---

Review $1 for correctness. Focus on ${2:-edge cases and error paths}.
```

Saved as `review.md`, that becomes `/review src/agent/driver.cpp`.

Templates are discovered from, in order:

1. `~/.clank/prompts/`
2. `<cwd>/.clank/prompts/`
3. the `promptTemplates` array in `~/.clank/settings.json`
4. `--prompt-template <path>`, repeatable

The name is the filename without `.md` — frontmatter cannot change it. Only
`description` and `argument-hint` are read, and neither is required; with no
description, the first body line (truncated to 60 characters) is used in the
command flyout. Directories are scanned non-recursively, and dotfiles are
skipped so editor lock files never load. As with skills, **the first root to
claim a name wins**, so a user-scoped template shadows a project one. Root 2 is
project-local and requires trust. `--no-prompt-templates` disables roots 1–3;
`--prompt-template` still loads. Built-in commands are matched first, so a
template cannot shadow `/compact`.

Arguments are split on whitespace, honoring `'` and `"` quoting (quotes are
stripped; there is no backslash escaping). Placeholders:

| Form | Expands to |
| --- | --- |
| `$1`, `$2`, … | that positional argument, or nothing if absent |
| `$ARGUMENTS`, `$@` | the full argument string |
| `${1:-text}` | `text` when argument 1 is missing or empty |
| `${ARGUMENTS:-text}` | `text` when no arguments were given |
| `${@:2}` | arguments 2 through the end, space-joined |
| `${@:2:3}` | three arguments starting at 2 |

`${1}` without a default is **not** a placeholder and is left as literal text,
matching pi. A template with no placeholders discards any arguments it is
given. Loaded templates appear in the TUI command flyout, and `/reload` re-runs
discovery.

```sh
./build/clank --prompt-template ./review.md   # load one template explicitly
./build/clank --no-prompt-templates            # skip all template discovery
```

## Tool selection

All eight tools (`read`, `write`, `edit`, `ls`, `find`, `grep`, `shell`,
`ask_user`) are enabled by default. The active set shapes the system prompt, so
disabling a tool also removes its snippet and guideline bullets.

```sh
./build/clank -t read,ls,grep      # allowlist: only these three
./build/clank -xt shell,write      # everything except these
./build/clank -nt                  # no tools at all
```

`--tools` (`-t`) and `--exclude-tools` (`-xt`) each take one comma-separated
list and may be repeated; exclusions are applied after the allowlist.
`--no-tools` (`-nt`) wins over both. Unknown tool names are an error rather
than being ignored, since without an extension mechanism an unrecognized name
is always a typo.

## Lifecycle hooks

Clank reports lifecycle transitions to external commands so an editor or
supervisor can follow a run it did not start, and lets one of those commands
decide whether a tool call is allowed to run at all. Hooks are configured in
`~/.clank/hooks.json` — deliberately **not** `settings.json`, which is rewritten
from its own known keys whenever a setting changes and would drop anything it
does not model.

```json
{
  "hooks": {
    "SessionStart": [
      { "hooks": [{ "type": "command", "command": "notify.exe", "timeout": 10 }] }
    ],
    "PreToolUse": [
      { "matcher": "read|write", "hooks": [{ "type": "command", "command": "audit.exe" }] }
    ]
  }
}
```

`matcher` is an optional `|`-separated alternation of exact tool names; `*`,
empty, or absent accepts everything. It is matched against the tool name, so it
only narrows `PreToolUse` and `PostToolUse` — a matcher on any other event is
ignored rather than silencing it. `timeout` is in seconds and defaults to 10.
Entries of an unrecognized `type`, and entries with no `command`, are skipped
rather than invalidating the file — one bad entry never disables the rest. A
missing `hooks.json` simply disables hooks; one that cannot be read or parsed
disables them too, but says so on stderr rather than failing silently.

| Event | Fires |
| --- | --- |
| `SessionStart` | once the session is durable, before the first prompt |
| `UserPromptSubmit` | each prompt submitted to the agent |
| `PreToolUse` | before a tool call runs, and decides whether it runs at all |
| `PostToolUse` | after a tool call completes |
| `Stop` | when an agent run finishes normally |
| `StopFailure` | when a run ends in an error; a user cancel reports `Stop` instead |
| `SessionEnd` | at exit |

Each command runs through the platform shell and receives a JSON payload on
**stdin**:

```json
{
  "hook_event_name": "PreToolUse",
  "session_id": "DuPy5yQe3amJZd0ZKOaUlw",
  "cwd": "C:\\work\\repo",
  "transcript_path": "C:\\Users\\me\\.clank\\sessions\\v2\\repo-c838...\\DuPy....jsonl",
  "tool_name": "read",
  "tool_call_id": "call_01",
  "tool_input": { "path": "src/main.cpp" },
  "interactive": true
}
```

`transcript_path` is reported directly so a consumer never has to re-derive the
project key. `tool_name`, `tool_call_id`, and `tool_input` are present only for
the tool events; `tool_input` is the canonical, already-validated arguments as a
nested object, not a string. If the arguments would push the payload past the
16 KiB stdin bound they are dropped whole and `"tool_input_omitted": true`
appears instead — half a JSON object is not a JSON object. `interactive` is
`true` when a user is present to answer, and appears only on `PreToolUse`.

The child never inherits Clank's stdin — the TUI holds the console in raw mode.
For every event other than `PreToolUse` its stdout, stderr, and exit code are
ignored: a hook is a one-way status report, and a broken one must not become an
agent failure.

Dispatch for those events is queued onto a worker thread, so a slow hook cannot
stall the agent loop or a TUI redraw, and events are dropped rather than
blocking if a burst outruns the queue. Every run-scoped event is dispatched from
the harness's own event stream, so hooks observe them in the order the agent
produced them — `UserPromptSubmit` before the tool events it caused, and `Stop`
after the last of them. All commands registered for an event run in
configuration order, with no cap on how many. On exit the backlog gets a bounded
grace period to finish before anything still outstanding is cancelled, and
`SessionEnd` is then reported synchronously so it is delivered even when
everything else was dropped. With no hooks configured no thread is started and
nothing is spawned, so the default footprint is unchanged. `--no-session` keeps
everything in memory and has no transcript to point at, so it reports none of
these events.

Switching sessions with `/clear` or `/resume` re-announces `SessionStart` with
the new session ID, transcript path, and working directory, so a consumer never
keeps tailing a log that stopped being written.

### The `PreToolUse` gate

`PreToolUse` is the one event that is a question rather than a report. It runs
**synchronously**, on the agent's own thread, during the clearance phase — every
call in a batch is asked before any of them starts — and its answer decides
whether the tool runs.

> **Breaking change.** `PreToolUse` used to be queued alongside every other
> event, with its output and exit code ignored. It is now blocking, and both are
> load-bearing. An existing `PreToolUse` hook that exits non-zero will start
> blocking tool calls.

A command answers in one of two ways, checked in this order:

1. **A JSON object on stdout carrying a boolean `block`.**
   `{"block": true, "reason": "...", "terminate": true}`. `reason` becomes the
   tool's error result verbatim, with no prefix; an empty or absent one falls
   back to `Tool execution was blocked`. `terminate` ends the run, but only if
   *every* call in the batch sets it. `terminate` without `block` is ignored.
2. **The exit code.** `0` allows; anything else blocks, with whatever the command
   printed to stdout as the reason.

A structured verdict outranks the exit code in both directions, so a hook can
allow while still reporting a non-zero status. That only applies to an object
with a real boolean `block`, though: `{"block": "true"}`, `{"decision": "deny"}`,
or any other schema is *not* a verdict, and falls through to the exit code rather
than being read as consent. Write the reason to **stdout**, not stderr: stderr
goes to the null device so a hook's diagnostics can never corrupt the decision
channel.

The gate **fails closed**. A command that cannot be spawned, outlives its
timeout, is cancelled, answers unintelligibly, or floods stdout with more than
64 KiB blocks the call and says why. A gate that fails open is not a gate — which
is also why it stays active under `--no-session`, unlike the reporting events.

Commands run in configuration order and the **first block wins**; the rest are
never spawned. `matcher` narrows the gate by tool name in the usual way, so a
tool no command matches costs nothing. Every gated call pays one process spawn,
bounded by that command's `timeout`.

Clank ships **no permission policy of its own** — no rules file, no path guards,
no dangerous-command patterns, no approval prompt. This is the same posture as
pi, which states plainly that it "does not include a built-in permission system"
and has "no permission popups. Run in a container, or build your own confirmation
flow." The gate is the mechanism; the policy is yours. A minimal one:

```sh
#!/bin/sh
# Block `shell` calls that look destructive; allow everything else.
payload=$(cat)
case "$payload" in
  *'rm -rf'*|*sudo*) echo '{"block":true,"reason":"blocked by local policy"}' ;;
  *)                 echo '{"block":false}' ;;
esac
```

## Compaction

Clank compacts automatically. Before each provider request the agent loop
compares the branch's context against the active model's window; once the
context has eaten into the reserved headroom it folds the older part of the
branch into a single summary and continues. The most recent messages are kept
verbatim, and the retained tail always begins at a user turn, so an assistant
tool call is never separated from its tool results.

Token pressure is read from the provider's reported usage when there is any
(prompt + cache + output of the newest assistant turn, plus an estimate for
anything appended since), and falls back to a byte-based estimate otherwise.

Every summary ends with the files the folded turns touched, taken from the
`path` argument of `read`, `write`, and `edit` tool calls:

```
<read-files>
docs/design.md
</read-files>

<modified-files>
src/agent/driver.cpp
</modified-files>
```

A path that was written or edited is reported only as modified. The lists are
stored on the compaction record as well as in the summary text, and each
compaction reseeds from the ones before it, so paths survive any number of
successive compactions even though the turns they came from are gone. Files
created by a `shell` command are not tracked, since only those three tools
publish a path argument.

Compaction runs at most once per turn; if it succeeds and the context is still
over budget, the request proceeds and the provider decides. If the compaction
turn itself fails, the run stops with an `automatic compaction failed` error
rather than letting an oversized request through.

Tune or disable it in `~/.clank/settings.json`:

```json
{
  "compaction": {
    "enabled": true,
    "reserveTokens": 16384,
    "keepRecentTokens": 20000
  }
}
```

`/compact` remains available and always folds the whole branch. It accepts
optional one-shot instructions — `/compact keep only the API decisions` — which
are appended to the summarization prompt for that compaction only. They are
never persisted and never applied to automatic compaction.

## Retries

Provider requests that fail with a retryable error are retried by the agent
loop. Retries are on by default: three attempts, 2 s base delay, doubling per
attempt (2 s, 4 s, 8 s) with no jitter, capped at 8 s. Tune or disable in
`~/.clank/settings.json`:

```json
{
  "retry": {
    "enabled": true,
    "maxRetries": 3,
    "baseDelayMs": 2000,
    "maxDelayMs": 8000
  }
}
```

Setting `maxRetries` to 0 or `enabled` to `false` fails on the first error. The
HTTP transport underneath does not retry independently, so these numbers are
the whole story.

## Turn limit

The agent loop keeps taking turns while the model keeps calling tools. A model
that never stops would otherwise run until the context fills or you cancel it,
so the loop carries a budget: after 100 consecutive turns with no input from
you, it stops and asks whether to keep going.

Answering *Continue* grants another full budget. Answering *Stop* — or
dismissing the question — ends the run cleanly with a notice saying how many
turns it took; the branch is intact and an ordinary follow-up prompt resumes
from exactly where it stopped. Steering or a follow-up message you send mid-run
also resets the budget, since the loop is no longer running unattended.

```json
{
  "turnLimit": {
    "enabled": true,
    "maxTurns": 100
  }
}
```

```sh
./build/clank --max-turns 25      # ask every 25 turns
./build/clank --max-turns 0       # never ask; the loop is unbounded again
```

When nobody can answer — under `-p`, `--mode json`, or a non-interactive
terminal — the run stops at the limit rather than blocking. Set `maxTurns` to
`0` or `enabled` to `false` for the old unbounded behavior.

## Asking the user

The `ask_user` tool lets the agent put a question to you mid-run and wait for
the answer, either free-form or as a fixed set of choices. It runs on its own,
never alongside another tool, so two questions can never compete for the
screen. With no user present it returns an error the model can read and work
around rather than blocking, which is what keeps `-p` and `--mode json` usable.

It is the same seam the turn limit uses, so a question looks identical
whichever raised it. In the TUI the modal is labelled `Question`, not
`Commands` — the breadcrumb reflects what opened the modal, so command-invoked
pickers like `/model` still read `Commands`.

## TUI

Running interactively on a real terminal launches a full-screen TUI: an
alternate-screen, double-buffered **differential renderer** (own cell grid +
scrollback), markdown-rendered assistant output (headings, lists, tables,
blockquotes, code blocks), a **sticky working-directory + git-branch line**
just above the prompt, a minimal status line along the bottom
(spinner and state on the left; model, supported effort/context, and token
usage right-aligned), and a **multi-line input editor**. The first frame is
painted synchronously before
HTTP initialization, cwd/git context discovery, and credential warmup; key input
starts before the remaining post-frame work. It runs three threads — the agent
loop, a key-input thread, and a render thread (~20fps) so the spinner stays live
while a turn waits on the network. Assistant deltas are displayed at complete
UTF-8 boundaries on the next render frame, without a separate typing-speed
throttle, so reveal speed is bounded only by provider delivery and rendering.

The interface is assembled with the internal, arena-backed `clank_ui`
component library. Layout, focus, hit-testing, pointer capture, reusable
widgets, and custom component integration are documented in
[`docs/UI.md`](docs/UI.md).

Keys: **Enter** sends · **Shift+Enter** / Ctrl+J / Alt+Enter inserts a newline ·
arrows move
(↑/↓ across lines, then through history at the edges) · **Ctrl+←/→** word jump ·
Home/End · **Shift** + movement selects composer text ·
**Ctrl+W/U/K** delete word / to start / to end · **mouse wheel** scrolls the
transcript · **left-drag** selects transcript or composer text (and auto-scrolls
the transcript) · an overflow-only scrollbar can be clicked or dragged (track
clicks center its thumb) · type during a turn to **steer** · **Ctrl+C** copies
selections via OSC 52 and confirms with a transient `copied N chars` message on
the status row; otherwise it interrupts a turn, cancels a command
operation, clears the composer, or quits · double-press **Escape** within 500 ms
to clear an idle composer or interrupt an active turn · hover or **left click**
a turn's first row to highlight or collapse/expand it · **Ctrl+O** expands all
tools when any is collapsed, otherwise collapses all · **Ctrl+D** quits ·
**Ctrl+L** repaints.
Honors `NO_COLOR`. Bracketed paste inserts literal multi-line text and replaces
a composer selection.

Shift+Enter needs a terminal that reports modified keys: entering the TUI
requests the kitty keyboard protocol (kitty, WezTerm, Ghostty, foot, Alacritty,
iTerm2, Windows Terminal 1.25+) and xterm's `modifyOtherKeys` level 2 (xterm,
VTE/GNOME Terminal, Konsole), and releases both on exit. Terminals that
implement neither — including legacy Windows conhost — cannot distinguish
Shift+Enter from Enter, so it submits there; Ctrl+J and Alt+Enter work
everywhere.

When stdin/stdout isn't a TTY (pipes, redirected) or with `-p`, it falls back to
a plain line renderer. Plain and piped output write provider deltas immediately
without display pacing. Session commands remain available; interactive
selectors such as `/login` and `/model` require a TTY.

Interactive commands:

- `/login` — choose an OAuth-capable provider, authenticate, then choose a model.
- `/model` — choose a provider and a live-discovered, fallback, or manual model
  ID, then choose the effort and context tier when that model exposes those
  controls. Fixed context windows are displayed without an unnecessary prompt.
- `/session` — inspect the current title, IDs, project, status, times, model,
  counts, token categories, and fork ancestry.
- `/name [title]` — set the current session name; an empty TUI value or
  `/name --clear` restores the first-user-message title.
- `/sessions [all]` — browse rich current-project metadata, or all projects.
- `/resume [id]` — choose an available session, including an all-project page,
  or switch directly by exact full ID.
- `/tree` and `/checkout <entry-id>` — inspect or navigate conversation branches.
- `/fork [entry-id]` — fork and switch to a new session.
- `/compact` — durably summarize the active branch (compaction also runs
  automatically; see Compaction above).
- `/trust` — save project trust for a future Clank process.
- `/reload` — transactionally reload settings and prompt context while idle.
- `/debug [on|off]` — toggle the performance overlay described under
  Debug overlay below. TUI only.
- `/rounded [on|off]` — toggle the rounded corners and bullets described under
  Rounded style below. TUI only; the choice is saved to `settings.json`.
- `/clear` — create a fresh session while preserving the provider, model,
  effort, and context selection.
- `/exit` (or `/quit`) — close Clank and return to the shell.

In the TUI, typing `/` opens a prefix-filtered command flyout over the content
directly above the composer. Provider, model, and manual-entry choices drill down
as pages in the same surface. Use Up/Down to select, Enter to continue or run the
command, Backspace on an empty page to go back, or Escape to dismiss the current
choice.

Commands entered during a TUI turn run afterward; ordinary text still steers the
active turn.

The selected provider/model, explicit effort/context preferences,
`defaultProjectTrust` policy, and the `rounded` UI style are written
atomically to `~/.clank/settings.json`.
New sessions use built-in defaults, then settings, then the runtime-only `-m`
override. Resumed sessions restore their durable provider/model ancestry; `-m`
adds a new durable model selection. Direct IDs are exact opaque durable IDs.
Cross-project resume enters the session's stored working directory, resolves
project trust again, rebuilds prompt context, and rollback-safely switches the
session, process cwd, and TUI cwd/branch. `--continue` remains scoped to the current project. Sessions
live under `~/.clank/sessions/v2/<project-key>/`. Canonical JSONL files also store names without
a sidecar. A writer holds a `<session-id>.lock` sentinel beside its log for as
long as it is open; the sentinel is removed on release, and on Windows the
kernel removes it even if the process is killed. A `<session-id>/` directory
beside the log holds that session's other artifacts, currently the `shell`
tool's output-overflow logs; it is created only when something needs it.
Unreadable session files are reported and skipped during
browsing/continuation. Interactive
mode may start without credentials and blocks prompts with `/login` guidance;
`-p` remains fail-fast.

Set `CLANK_PRINT_MEM=1` to print peak RSS at exit.

### Debug overlay

`/debug` toggles a performance panel in the top-right corner of the TUI;
`/debug on` and `/debug off` set it explicitly. It reports the frame rate, the
mean and worst frame interval over the last 64 frames, and — for the most recent
turn — time to first token, wall time, token usage, end-to-end output tokens per
second, and the last tool's duration. It also reports current process resident
memory (RSS).

The panel only paints. It never takes focus or receives input, and modals and
the command flyout draw over it. Timing samples accumulate while the panel is
hidden, so the window is already full when it is switched on. RSS is queried
when the panel appears and at most once per second while it remains visible.
The state is per-run and always starts off — nothing about it is written to
`settings.json`.

### Rounded style

`/rounded on` uses `╭ ╮ ╰ ╯` borders and `●`/`○` transcript bullets.
`/rounded off` uses `┌ ┐ └ ┘` and `◼`/`◻`. The choice is stored as `"rounded"`
in `~/.clank/settings.json`; `/reload` applies edits made to the file.

## Authentication

Two credential types, stored in `~/.clank/auth.json` (atomic writes,
restricted perms). The resolved token and OAuth expiry are cached in the
running app; expiry, login, provider changes, and `/reload` invalidate or
replace the cache so OAuth refresh still works mid-session.

- **API key** — set `ANTHROPIC_API_KEY` or `OPENAI_API_KEY`, or store a key in
  `auth.json`.
- **OAuth (subscription)** — `clank --login anthropic` runs an
  authorization-code + PKCE flow: it opens your browser to approve access and a
  local callback server on `http://localhost:53692/callback` captures the
  redirect automatically (no copy/paste). The browser shows a success page; the
  access/refresh tokens are saved and auto-refreshed. Pass `--no-open` to print
  the URL instead of launching a browser. `clank --login openai` uses the same
  browser flow on OpenAI's callback port for a ChatGPT subscription.
  Interactive `/login` supports both providers. `--logout <provider>` removes
  credentials.

OpenAI API-key requests use the public Responses API. ChatGPT subscription
requests use the backend protocol implemented by OpenAI's official Codex
client. That OAuth/backend surface is not a stable public API, so it is isolated
from the public API path and covered by separate fixtures.

The OAuth flow/storage is **provider-generic** (`src/auth/`): a config table
(`OAuthConfig`) plus generic PKCE/exchange/refresh. Only how the token is applied to a request is per-provider. Anthropic OAuth uses
the Claude subscription endpoint conventions; OpenAI OAuth uses the ChatGPT
Responses backend conventions, including the account ID carried by the access
token.

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

Suites tagged `network` (currently `http_live`, a live TLS/HTTP request that replaces
the old `clank_http_smoke` binary) are skipped by default, so a bare run stays
offline and deterministic. Request one by name or with `--all`; point it at a
local server with `CLANK_TEST_HTTP_URL`. Exit code is 0 on success, 1 on test
failure, 2 on a usage error. `ctest` runs the offline sweep as the `unit` test;
the `network` test is registered but disabled by default.

Adding a suite is a new `tests/test_*.cpp`, a declaration in `tests/tests.h`, and
one row in the `kSuites` table in `tests/test_main.cpp`.

The default run is network-free and key-free: the loop runs against the
scripted provider in `tests/script_provider.h`, the Anthropic SSE parser against a recorded fixture, the OAuth module
against fixtures, the callback server against an in-process loopback socket, and
the TUI via pure units (key decoder, differential-renderer diff, markdown,
line-wrapping, the multi-line editor) plus a headless controller test driving the
event→transcript mapping. Live full-screen rendering is verified manually.

## JSON mode

`--mode json` turns print mode into a machine surface: one JSON object per line
on stdout, flushed as it happens. All human-facing output stays on stderr, so
stdout is safe to pipe.

```sh
clank --mode json -p "summarize README.md"
```

```json
{"type":"agent_start","turn":"run"}
{"type":"record","kind":"message","record":{...}}
{"type":"turn_start","turn":"run"}
{"type":"text_delta","index":0,"text":"pong"}
{"type":"turn_end","turn":"run","outcome":"succeeded"}
{"type":"agent_end","turn":"run","outcome":"succeeded"}
{"type":"result","ok":true}
```

Event types: `agent_start`, `agent_end`, `turn_start`, `turn_end`,
`text_delta`, `thinking_delta`, `tool_call_delta`, `tool_start`, `tool_update`,
`tool_end`, `retry_start`, `retry_end`, `queue_update`, `cancel_requested`,
`record`, and a final `result`.

`record` carries a durable session entry verbatim, in the same schema the
session log stores, so a consumer sees exactly the state that was persisted
rather than a re-flattened copy. `agent_end` always arrives before `result`,
and `result.ok` matches the process exit code. A failed run adds
`result.error`; a run that succeeded but stopped early — today only at the turn
limit — adds `result.message` instead.

`--mode json` requires `-p`; there is no interactive form without a
bidirectional protocol.

## Scope / deferred

This is the small clean core.

Deliberately out of scope for now (the structure leaves seams for them):

- Additional OpenAI-compatible or vendor-specific providers.
- A bidirectional RPC mode. `-p` with `--mode text|json` is the non-interactive
  surface; there is no stdin-driven protocol yet.
- Device-code OAuth (e.g. GitHub Copilot) — the loopback callback flow is done.
- TUI niceties: visual-row (vs logical-line) Up/Down, syntax highlighting,
  images, undo. The full-screen differential TUI itself is done.
- Skills packages (`pi install`-style distribution), general extensions, and
  additional providers. Local skill and prompt-template discovery is done.
- pi's two-phase split-turn summary for a single turn that exceeds the
  retained-tail budget. One-shot `/compact <instructions>` is done.
- Fuzzy/regex session search, generated names, pricing, deletion UX, and a
  derived session metadata sidecar.

## License

MIT (matching the upstream pi project this core is modeled on).
