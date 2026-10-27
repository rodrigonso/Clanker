# Clanker

A tiny, memory-efficient coding-agent harness in C++23 — a clean reimplementation
of the core of [`earendil-works/pi`](https://github.com/earendil-works/pi)
without a JavaScript runtime.

Supported platforms are **Windows and macOS**, for both the CLI and core SDK.

The goal is footprint. A Node/Bun-based agent pays for an embedded JS engine and
SDK object graphs before it does any work, and duplicates the transcript several
times in UTF-16. Clanker runs the same agent loop with an arena-allocated, UTF-8,
zero-copy transcript.

**Measured** on Windows x64 with `CLANKER_PRINT_MEM=1`: a complete live run
(libcurl + TLS + one prompt and response over HTTPS) peaks at **~12.4 MB RSS**
from a Release build. For scale, an empty `node -e ''` process
on the same machine (Node 24.13.0) sits at ~33 MB before loading an agent at
all. The claim here is the order of magnitude, not a benchmarked ratio against
a specific upstream measurement — pi publishes no memory numbers.

## Install and run

Clanker is a **public-alpha candidate**. Release automation prepares unsigned
previews; it does not automatically publish them. If no release is listed on
this repository's **Releases** page yet, use [Build](#build).

```powershell
# Windows 10+ (x86_64)
irm https://raw.githubusercontent.com/rodrigonso/Clanker/main/tools/install/install.ps1 | iex
```

```sh
# macOS 15+ (Apple Silicon or Intel)
curl -fsSL https://raw.githubusercontent.com/rodrigonso/Clanker/main/tools/install/install.sh | sh
```

The install script downloads the newest release archive for your system,
verifies its `.sha256` checksum, and installs it with its license notices. It
needs no administrator rights, language runtime, or helper tools.

| System | Installs to | On `PATH` via |
|---|---|---|
| Windows | `%LOCALAPPDATA%\Programs\clanker` | user `PATH` (open a new terminal) |
| macOS | `~/.local/share/clanker` | `~/.local/bin/clanker` symlink; the script prints the `export` line if that directory is not on `PATH` |

Set `CLANKER_INSTALL_VERSION=X.Y.Z` to pin a release, `CLANKER_INSTALL_DIR` to
install elsewhere, or (Windows) `CLANKER_NO_MODIFY_PATH=1` to leave `PATH`
alone. `clanker upgrade [--version X.Y.Z]` reruns the script against the running
copy's install directory; it is a no-op when that version is already installed.
An interactive Clanker started from a script install checks GitHub for a newer
release at most once a day in the background and mentions it on a later start;
set `CLANKER_NO_UPDATE_CHECK=1` to turn that off. Nothing is downloaded until you
run `clanker upgrade`.

You can also download a `clanker-<version>-<system>.zip`/`.tar.gz` archive and
its `.sha256` from **Releases**, extract it anywhere, and run `bin/clanker`
directly; such copies do not self-upgrade.

Start Clanker **from your project directory**:

```sh
cd ~/work/my-project
clanker --permission-mode ask
```

Follow the first-run setup; [Providers](#providers) describes configuration.
Start in Ask mode and review tool requests. Clanker can read and modify project
files and run shell commands; conversation and tool context are sent to the
selected model provider. Provider access and model usage are separate from the
local executable. Ask mode is an approval policy, not an OS sandbox.

**Preview limitations:** macOS native qualification is pending the native CI
runs and interactive release checks. Some Windows filesystem/process operations
still require ASCII installation, profile, and workspace paths. Archives are
unsigned (and not notarized on macOS), so the OS may warn or block first launch.
Do not disable OS-wide protections to run an untrusted download. SHA256
checksums detect corruption, not publisher identity.

Maintainers: see [release preparation](docs/RELEASING.md) for the native build
matrix, draft-release process, and remaining publication gates.

## What it is

A runnable Clanker agent: the agent loop + an OpenAI Responses API provider
(HTTP/SSE) + `read`, `write`, `edit`, `ls`, `find`, `grep`,
`shell`, and `ask_user` tools + a streaming REPL.

```
src/
  base/    Arena (cross-platform reserve/commit), Array<T>, UTF-8 Str/StrBuf,
           FFI fastfail, peak-RSS reporter   (caboose conventions, no C++ std)
  json/    arena DOM parser + escaping writer (hand-rolled, no deps)
  crypto/  base64, secure random (no deps)
  llm/     data model, OpenAI Responses protocol
           (request build + SSE parse), retry policy, model registry
  net/     streaming HTTP + non-streaming fetch over libcurl
  auth/    credential store (api keys) + resolution/caching + OpenRouter
           OAuth PKCE
  session/ append-only v2 message tree, reducer, memory + JSONL repositories
  agent/   incremental AgentHarness, in-memory queues, hooks, events, compaction
  tools/   read/write/edit/ls/find/grep/shell/ask_user, direct/shell process
           exec, in-process search, fs/glob/diff/truncate/filelock infra
  term/    full-screen TUI: raw-mode terminal, key decoder, differential
           renderer, markdown, transcript model, multi-line editor, controller
  app/     CLI entry + durable session startup + TUI/plain REPL + credential
           resolution + external lifecycle hooks (hooks.json config + dispatch
           runner)
tests/     single CLI test harness (suite selection via args; offline by default)
```

## Design notes

- **No C++ standard library**, no exceptions, no RTTI. Types and conventions
  (`Arena`, `Array<T>`, PascalCase fields, `FFI(cond)` fastfail) follow the
  caboose/Briq house style. The CLI uses the C runtime, platform APIs, and the
  C APIs of libcurl and PCRE2.
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
- **One reimplemented wire protocol**, not vendor SDKs. Clanker speaks
  `POST /v1/responses` and nothing else; every provider is a row of
  endpoint data rather than a protocol implementation. The request builder and
  SSE parser are tested against recorded fixtures offline. See
  [Providers](#providers).
- **External lifecycle hooks** let an editor or supervisor follow a run without
  linking against Clanker. See [Lifecycle hooks](#lifecycle-hooks).

## Build

`build.bat` and `tools/build.sh` are thin wrappers around **CMake 3.24+**.
The root `CMakeLists.txt` owns source lists, compiler settings, targets, and SDK
installation. `cmake/curl.cmake` and `cmake/pcre2.cmake` configure the CLI's
pinned source dependencies; release packaging stays in separate scripts.

Windows requires the Visual Studio Desktop development with C++ workload,
including the Windows SDK and C++ CMake tools (CMake and Ninja). The script
locates and initializes the x64 toolchain automatically; no developer prompt
is needed. macOS requires CMake, a C++23-capable toolchain, and Make or Ninja.
These are build-machine requirements, not runtime dependencies. Unsupported
hosts and targets are rejected before configuring compilers or dependencies.

C++23 is required for all Clanker targets and core SDK consumers. CMake selects
the appropriate language-mode flag for the detected compiler.

Windows Release, RelWithDebInfo, and MinSizeRel builds statically link the CRT
(`/MT`) throughout Clanker, libcurl, and PCRE2. Debug builds retain the shared
debug CRT (`/MDd`). Reconfigure and rebuild existing build directories when
changing this runtime policy.

The CLI builds SHA256-pinned static libcurl and PCRE2 10.48 (the 8-bit C regex
library) using their upstream CMake builds.
Downloaded sources and dependency build outputs stay in the selected build
directory under `_deps/`. HTTPS uses native Schannel on Windows and
SecureTransport on macOS, with the host certificate trust store.
Core-only builds never fetch or build networking or regex dependencies.

```bat
build.bat                                  REM Release CLI
build.bat -Config Debug                     REM Debug CLI
build.bat -Test                             REM full offline sweep
build.bat -Test -TestArgs session,tools      REM selected suites
build.bat -CoreOnly -Examples -Test          REM dependency-free core
```

The same options work with `.\tools\build.ps1` from PowerShell.

The `grep` tool searches in-process and requires no external search executable.
Normal use does not download or install helper tools, libraries, or runtimes.
Regex patterns use PCRE2 syntax: common existing search options are preserved,
but this is not a byte-for-byte clone of ripgrep's Rust regex dialect.
Regex work is bounded; exceeding a matching limit returns an explicit tool error.
Explicit regex callouts and `\C` are rejected.

Files are streamed through one reusable line buffer. UTF-8 and BOM-marked UTF-16
are supported; NUL-containing binary files are skipped. Individual decoded lines
over 8 MiB fail explicitly rather than exhausting arena memory. Output defaults
to 100 matches and is capped at 50 KiB, with displayed lines truncated to 500
characters. Directory traversal does not recursively follow symlinks.
Search paths and filenames use UTF-8, including Windows Unicode descendants.
Negative directory globs prune their subtrees; brace alternatives and character
classes are supported, but nested brace alternatives are rejected explicitly.
Ignore handling reads `.gitignore` and `.ignore`, including parent scopes; Git's
global excludes and `.git/info/exclude` are not currently loaded.

```sh
bash tools/build.sh              # macOS CLI
bash tools/build.sh --test       # build + offline suites
```

CMake handles incremental and parallel builds, including header dependencies.
`-Clean` / `--clean` rebuilds without deleting the build directory or downloaded
sources. `-CurlArchive` / `--curl-archive` and `-Pcre2Archive` / `--pcre2-archive`
accept offline source archives; their SHA256 values must match the pins in
`cmake/curl.cmake` and `cmake/pcre2.cmake`, respectively. These set the CMake cache
options `CLANKER_CURL_ARCHIVE` and `CLANKER_PCRE2_ARCHIVE`. Use separate build
directories when changing compilers or sharing a checkout across platforms.
Archive pins are checked again on every configure, even when their sources
have already been populated.

The build exposes three options: `CLANKER_BUILD_CLI` (on by default),
`CLANKER_BUILD_TESTS`, and `CLANKER_BUILD_EXAMPLES` (both off by default).
The corresponding targets are `clanker`, `clanker_core`, `clanker_tests`, and
`clanker_core_example`. One private object target shares CLI code with its
tests; there is no hierarchy of component libraries or CPack configuration.

`tools\run.ps1` builds incrementally and then launches the executable in your
current directory:

```powershell
.\tools\run.ps1                       # build, then start the TUI
.\tools\run.ps1 -- -c                 # forward Clanker's own flags after --
.\tools\run.ps1 -NoBuild --help       # skip the build, run what's there
```

Put `--` before Clanker's short options: without it PowerShell binds `-c` to
`run.ps1`'s own `-Config`.

### Dependency-free core SDK

The core contains the agent loop, permissions, compaction, JSON, memory and
durable session repositories, filesystem helpers, and platform primitives.
It uses only the C runtime and OS APIs: **no libcurl, PCRE2,
JavaScript runtime, or dependency downloads**. Providers and tools are supplied
by the embedding application. The bundled provider, tools, terminal UI, and
application settings are CLI features, not part of this SDK.

```powershell
.\tools\build.ps1 -CoreOnly -Examples -Test -InstallPrefix .\out\sdk
```

```sh
bash tools/build.sh --core-only --examples --test --install-prefix "$PWD/out/sdk"
```

Core-only builds default to `out/core`; use a separate `--build-dir`/`-BuildDir`
when sharing a checkout between operating systems. Both modes use the same
`clanker_tests` runner; a core-only build registers just its 14 core suites.
`--examples`/`-Examples` also runs the offline provider-injection example.

Installation supplies `include/clanker` and a single static core archive under
`lib`: `clanker_core.lib` on Windows, `libclanker_core.a` on macOS. Debug archives
have a `d` suffix so Debug and Release can coexist. Consumers use their own build
system; no generated project or package-manager integration is required.

[`examples/core/main.cpp`](examples/core/main.cpp) creates an in-memory session,
injects a local `Provider::Stream`, runs the harness, and reads its resulting
transcript without making any network requests. To compile it against the
installed SDK, use an x64 Visual Studio Developer Command Prompt. The manual
command uses `/std:c++latest` for MSVC's C++23 support:

```bat
cl /nologo /std:c++latest /MT /GR- /EHs-c- /Iout\sdk\include\clanker ^
  examples\core\main.cpp /Foout\consumer.obj /Feout\consumer.exe ^
  /link out\sdk\lib\clanker_core.lib psapi.lib bcrypt.lib
out\consumer.exe
```

Windows SDK consumers must match the archive's CRT: `/MT` for Release,
RelWithDebInfo, and MinSizeRel, or `/MDd` with `clanker_cored.lib` for Debug.

On macOS:

```sh
c++ -std=c++23 -fno-exceptions -fno-rtti -Iout/sdk/include/clanker \
  -c examples/core/main.cpp -o out/consumer.o
cc out/consumer.o out/sdk/lib/libclanker_core.a -pthread -lm -o out/consumer
./out/consumer
```

The provider owns its context; returned messages must use the arena passed to
`Stream`, and streaming slices need only remain valid during the callback.
Keep configuration backing storage alive for the harness lifetime. Close the
harness before its repository and release caller-owned arenas afterward.
Embedding defaults differ intentionally from the CLI: turn limits and compaction
are disabled, and tools are admitted in Yolo mode unless configured otherwise.
The example selects Ask explicitly and registers no tools.

This is an early C++ source API, not a stable ABI. Rebuild the SDK and consumers
together with compatible compiler/runtime settings when upgrading.

### CLI distribution

The CLI statically links libcurl and PCRE2 and keeps platform TLS. The build
scripts produce a relocatable runtime-only ZIP on Windows or TGZ on macOS,
plus a companion SHA256 checksum, under the build directory's `packages` folder.
SDK headers and libraries are not included in these CLI archives.

```powershell
.\tools\build.ps1 -Package
.\tools\build.ps1 -InstallPrefix .\out\install
```

On macOS:

```sh
bash tools/build.sh --build-dir out/release --package
```

The packaging step requires the project and upstream notices and rejects Debug
configurations and unresolved or non-system
runtime dependencies. Windows Clanker binaries statically link the release CRT
and require no VC runtime DLLs; packaging rejects a Clanker binary that imports
them. Windows 10+ and its certificate store are still required. macOS uses
system frameworks; the unsigned preview targets macOS 15+ in native CI.
Native CI and interactive qualification must pass before publishing. Existing
Windows ANSI filesystem/process operations still require ASCII installation
and workspace paths.

Each archive contains `share/clanker/DISTRIBUTION.txt` and dependency notices;
that file also marks a script install, which `clanker upgrade` requires. The
install scripts (`tools/install/`) take a local archive through
`CLANKER_INSTALL_ARCHIVE`, which is how native CI exercises them. These
scripts do not sign, publish, or include user configuration or session data.
Missing project or upstream notices are packaging errors. The GitHub workflow
can prepare a draft release after all native jobs succeed; publishing remains
a maintainer action. See [release preparation](docs/RELEASING.md).

### Visual Studio

The checked-in `.vsconfig` declares the Desktop development with C++ workload
and CMake tools. Open the folder in Visual Studio to use its CMake integration,
or build with `build.bat -Config Debug` and launch or attach the debugger to
`build\clanker.exe`. There are no project-specific CMake presets to maintain.

## Run

```sh
./build/clanker -p "hi"                # set OPENROUTER_API_KEY
./build/clanker                        # full-screen TUI; /model picks a live model
./build/clanker -m z-ai/glm-4.6 -p "summarize README.md"
./build/clanker --permission-mode ask -p "review the working tree"
./build/clanker --mode json -p "summarize README.md"   # NDJSON event stream
./build/clanker --continue             # most recent session for this project
./build/clanker --resume SESSION_ID     # exact ID, including sessions from other projects
./build/clanker --no-session            # no session filesystem writes
./build/clanker --system-prompt SYSTEM.md -p "review this project"
./build/clanker --append-system-prompt "Prefer minimal changes"
./build/clanker --no-context-files       # skip AGENTS.md / CLAUDE.md discovery
./build/clanker --no-skills              # skip SKILL.md discovery
./build/clanker --skill ./my-skill       # load one skill explicitly (repeatable)
./build/clanker --no-prompt-templates    # skip prompt-template discovery
./build/clanker --prompt-template ./review.md   # load one template (repeatable)
./build/clanker -t read,ls,grep          # allowlist of tools to enable
./build/clanker -xt shell                # disable specific tools
./build/clanker --no-tools               # read-only conversation
./build/clanker --max-turns 25           # ask to continue every 25 turns (0: unlimited)
./build/clanker --approve                # trust project prompt files for this run
```

## Project context and system prompts

Clanker builds the system prompt from the active tool catalog, project
instructions, and cwd. It loads the first regular file named
`AGENTS.override.md`, `AGENTS.md`, `AGENTS.MD`, `CLAUDE.md`, or `CLAUDE.MD`
from `~/.clanker` and each directory from the filesystem root through cwd.
`AGENTS.override.md` therefore wins over the other candidates in its own
directory, while every other directory still layers normally. Global context is
applied first, followed by broad-to-specific project context. Use
`--no-context-files` (`-nc`) to disable this discovery.

Tools contribute their own prompt text: a one-line snippet for the `Available
tools` list and optional guideline bullets that are merged, in tool order, with
the always-on guidelines. A tool without a snippet is not listed.

`~/.clanker/SYSTEM.md` replaces the built-in prompt globally, and
`~/.clanker/APPEND_SYSTEM.md` appends instructions. Trusted projects can override
those files with `<cwd>/.clanker/SYSTEM.md` and
`<cwd>/.clanker/APPEND_SYSTEM.md`. `--system-prompt` and repeatable
`--append-system-prompt` values have highest precedence; each value is read as
a file when it names an existing regular file, otherwise it is treated as
literal text. Custom prompts still receive project context and cwd.

Project prompt overrides require trust. Decisions are stored hierarchically in
`~/.clanker/trust.json`; the nearest saved ancestor wins. Set
`defaultProjectTrust` in `~/.clanker/settings.json` to `ask` (default), `always`,
or `never`. `--approve` (`-a`) and `--no-approve` (`-na`) override trust for one
run. `/trust` saves a decision for the current or parent folder and requires a
restart; `/reload` reloads settings and prompt/context files while preserving
the current process's resolved trust. If gated project prompt files first
appear after startup, `/reload` fails closed and requires a restart unless an
explicit CLI trust override covers the run.

`~/.clanker/settings.json` also stores the active provider/model and explicit
effort or selectable context-window preferences. Unsupported saved values fall
back to the selected model's defaults. Current fixed provider context windows are displayed without an extra chooser.

## Skills

Clanker implements the [Agent Skills](https://agentskills.io/specification)
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

1. `~/.clanker/skills/`
2. `~/.agents/skills/`
3. `.agents/skills/` in each directory from the repository root through cwd
4. `<cwd>/.clanker/skills/`
5. the `skills` array in `~/.clanker/settings.json`
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
./build/clanker --skill ./tools/review-skill      # load one skill explicitly
./build/clanker --no-skills                        # skip all skill discovery
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

1. `~/.clanker/prompts/`
2. `<cwd>/.clanker/prompts/`
3. the `promptTemplates` array in `~/.clanker/settings.json`
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
./build/clanker --prompt-template ./review.md   # load one template explicitly
./build/clanker --no-prompt-templates            # skip all template discovery
```

## Tool selection

All eight tools (`read`, `write`, `edit`, `ls`, `find`, `grep`, `shell`,
`ask_user`) are enabled by default. The active set shapes the system prompt, so
disabling a tool also removes its snippet and guideline bullets.

```sh
./build/clanker -t read,ls,grep      # allowlist: only these three
./build/clanker -xt shell,write      # everything except these
./build/clanker -nt                  # no tools at all
```

`--tools` (`-t`) and `--exclude-tools` (`-xt`) each take one comma-separated
list and may be repeated; exclusions are applied after the allowlist.
`--no-tools` (`-nt`) wins over both. Unknown tool names are an error rather
than being ignored, since without an extension mechanism an unrecognized name
is always a typo.

## Lifecycle hooks

Clanker reports lifecycle transitions to external commands so an editor or
supervisor can follow a run it did not start, and lets one of those commands
decide whether a tool call is allowed to run at all. Hooks are configured in
`~/.clanker/hooks.json` — deliberately **not** `settings.json`, which is rewritten
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
  "transcript_path": "C:\\Users\\me\\.clanker\\sessions\\v2\\repo-c838...\\DuPy....jsonl",
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

The child never inherits Clanker's stdin — the TUI holds the console in raw mode.
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

Clanker includes a built-in permission policy for tool calls. Select one of three
modes with `--permission-mode` for a single run, or `/permissions` inside an
interactive session:

- **Ask** runs inspection tools directly and prompts before writes, edits,
  shell commands, and unknown tools.
- **Auto** also admits workspace-local writes/edits and a narrow reversible
  shell allowlist. Every other action goes to the configured reviewer model,
  which is an extra billed request. A non-allow becomes a failed tool result
  carrying an opaque request ID; the model must pass that exact ID to
  `ask_user` to open the real approval prompt.
- **Yolo** bypasses the built-in checks and reviewer, but not external
  `PreToolUse` hooks. An unacknowledged Yolo activation warns prominently.

`Always for this session` approvals are held in memory and last only for the
current harness session. `/permissions reset` clears those grants and restores
Ask. New installations default to Auto with reviewer `z-ai/glm-5.2`; both the
global `permissionMode` and `permissionReviewerModel` are configurable in
`~/.clanker/settings.json`. `/permissions` reports the active mode, reviewer,
and grant count. In the TUI, **Shift+Tab** cycles
`ask -> auto -> yolo -> ask`; the footer shows the active mode. JSON print mode
emits `permission_review_start` and `permission_review_end` around billed
review calls.

These controls are separate from project trust and `PreToolUse` hooks. Project
trust decides whether prompt-resource files are loaded; tool permissions decide
whether a tool call is admitted; `PreToolUse` is an independent external gate
that can still block a call. Clanker provides no sandbox: approved tools run
with the process user's filesystem and OS privileges. In headless mode there is
no human approval path, so Ask and an unavailable Auto reviewer fail closed.
This feature does not attempt filesystem sandboxing, persistent built-in rule
files, or network isolation.

The external hook gate remains available for policies that need those controls.
A minimal one:

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

Clanker compacts automatically. Before each provider request the agent loop
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

Tune or disable it in `~/.clanker/settings.json`:

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
`~/.clanker/settings.json`:

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
./build/clanker --max-turns 25      # ask every 25 turns
./build/clanker --max-turns 0       # never ask; the loop is unbounded again
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

The interface is assembled with the internal, arena-backed `clanker_ui`
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
selectors such as `/auth` and `/model` require a TTY.

Interactive commands:

- `/auth` — sign in to OpenRouter in the browser, then choose a model.
- `/model` — choose a live-discovered or manual OpenRouter model ID, then choose
  the effort and context tier when that model exposes those controls. Fixed
  context windows are displayed without an unnecessary prompt.
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
- `/trust` — save project trust for a future Clanker process.
- `/permissions [ask|auto|yolo|reset]` — inspect or change tool permissions;
  `reset` clears session grants and restores Ask.
- `/reload` — transactionally reload settings and prompt context while idle.
- `/debug [on|off]` — toggle the performance overlay described under
  Debug overlay below. TUI only.
- `/rounded [on|off]` — toggle the rounded corners and bullets described under
  Rounded style below. TUI only; the choice is saved to `settings.json`.
- `/clear` — create a fresh session while preserving the provider, model,
  effort, and context selection.
- `/exit` (or `/quit`) — close Clanker and return to the shell.

In the TUI, typing `/` opens a command flyout over the content directly above
the composer. Its query matches command names, aliases, and descriptions using
case-insensitive substrings. In every selection page, typing silently filters
the complete displayed option labels without showing a query field. Use Up/Down
to select, Enter to continue or run the command, Backspace to edit the hidden
query (or go back when it is empty), or Escape to dismiss the current choice.

Commands entered during a TUI turn run afterward; ordinary text still steers the
active turn.

The selected provider/model, explicit effort/context preferences,
`defaultProjectTrust` policy, permission mode and reviewer model, and the
`rounded` UI style are written atomically to `~/.clanker/settings.json`.
New sessions use built-in defaults, then settings, then the runtime-only `-m`
override. Resumed sessions restore their durable provider/model ancestry; `-m`
adds a new durable model selection. Direct IDs are exact opaque durable IDs.
Cross-project resume enters the session's stored working directory, resolves
project trust again, rebuilds prompt context, and rollback-safely switches the
session, process cwd, and TUI cwd/branch. `--continue` remains scoped to the current project. Sessions
live under `~/.clanker/sessions/v2/<project-key>/`. Canonical JSONL files also store names without
a sidecar. A writer holds a `<session-id>.lock` sentinel beside its log for as
long as it is open; the sentinel is removed on release, and on Windows the
kernel removes it even if the process is killed. A `<session-id>/` directory
beside the log holds that session's other artifacts, currently the `shell`
tool's output-overflow logs; it is created only when something needs it.
Unreadable session files are reported and skipped during
browsing/continuation. Interactive
mode may start without credentials and blocks prompts with `/auth` guidance;
`-p` remains fail-fast.

Set `CLANKER_PRINT_MEM=1` to print peak RSS at exit.

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
in `~/.clanker/settings.json`; `/reload` applies edits made to the file.

## Providers

Clanker supports one provider: OpenRouter. It speaks the Responses API shape
(`POST <base>/responses`, SSE `response.*` events), while OpenRouter translates
that protocol for the upstream model vendor.

| Provider | Default base URL | API key env | Base URL env |
| --- | --- | --- | --- |
| `openrouter` | `https://openrouter.ai/api/v1` | `OPENROUTER_API_KEY` | `OPENROUTER_BASE_URL` |

`openrouter/auto` is the bootstrap default. It is OpenRouter's stable automatic
router, not a compiled model-availability table.

```sh
OPENROUTER_API_KEY=... \
  ./build/clanker -m anthropic/claude-sonnet-4.5 -p "summarize README.md"
```

`/model` fetches OpenRouter's live `GET /api/v1/models` catalog. Models enter
the picker only when the catalog advertises tool use; advertised modalities
must also include text input and text output. Context windows, reasoning
support, and reasoning-effort tiers come from that response. Clanker has no
compiled model list and never infers capabilities from model IDs. If discovery
is unavailable, manual model-ID entry remains available.

Why Responses and not Chat Completions: the models Clanker targets reject
`reasoning_effort` alongside function tools on `/v1/chat/completions`, and Chat
Completions standardizes no way to return thinking to the server. Responses
accepts both, and `include: ["reasoning.encrypted_content"]` with `store: false`
lets Clanker keep owning the transcript while still replaying reasoning across
turns.

Deliberate limits:

- **No subscription OAuth.** Subscription tokens (ChatGPT, Claude Pro/Max) are
  issued for other services. An OpenRouter API key is the only credential;
  `/auth` obtains one through OpenRouter's OAuth flow.
- **Reasoning replay is model-scoped.** Encrypted reasoning and item IDs are
  only valid for the model that produced them, so a mid-session model switch
  drops prior reasoning and regenerates IDs rather than replaying stale ones.
- **No direct vendor transports.** Model reach and routing are whatever the
  current OpenRouter catalog and account expose.

## Authentication

Credentials are stored in `~/.clanker/auth.json` (atomic writes, restricted
perms). The resolved token is cached in the running app; storing a key, provider
changes, and `/reload` invalidate or replace the cache.

- **Sign in** — run `/auth` in the TUI. Clanker opens your browser to
  OpenRouter, where you approve it; OpenRouter then mints an API key labeled
  `Clanker` on your account (OAuth PKCE), which Clanker stores. The redirect is
  received by a one-shot listener on `127.0.0.1`. Under SSH, or when no
  browser can be opened, Clanker prints the sign-in URL instead and you paste
  the code OpenRouter shows. Revoke the key from your OpenRouter key settings.
- **Environment** — set `OPENROUTER_API_KEY` for `-p`, CI, and other headless
  runs. A credential is always required.

Resolution priority: stored api_key → env var. `--logout <provider>` removes
credentials.

## Test

`clanker_tests` is the single test harness for the whole project: one CLI that
lets you (or an agent) pick exactly what to exercise.

```sh
./build/clanker_tests                 # every offline suite
./build/clanker_tests --list          # suite names, tags, descriptions
./build/clanker_tests json session    # only the named suites
./build/clanker_tests --filter line   # substring match: linemodel + lineedit
./build/clanker_tests --all           # include network-tagged suites
./build/clanker_tests --help          # usage
```

Suites tagged `network` (currently `http_live`, a live TLS/HTTP request that replaces
the old `clanker_http_smoke` binary) are skipped by default, so a bare run stays
offline and deterministic. Request one by name or with `--all`; point it at a
local server with `CLANKER_TEST_HTTP_URL`. Exit code is 0 on success, 1 on test
failure, 2 on a usage error.

Adding a suite is a new `tests/test_*.cpp` and one row in `tests/suites.txt`.
That registration supplies the source file, function declaration, and runner
entry. Mark CLI-only suites `cli`; the core build includes only `core` rows.
Unfiltered build-script runs also exercise CMake's unsupported-platform guard
for both the CLI and core SDK, before any compiler or dependency setup.

The default run is network-free and key-free: the loop runs against the
scripted provider in `tests/script_provider.h`, the Responses request
builder and SSE parser against recorded fixtures, and the TUI via pure units
(key decoder, differential-renderer diff, markdown, line-wrapping, the
multi-line editor) plus a headless controller test driving the
event→transcript mapping. Live full-screen rendering is verified manually.

## JSON mode

`--mode json` turns print mode into a machine surface: one JSON object per line
on stdout, flushed as it happens. All human-facing output stays on stderr, so
stdout is safe to pipe.

```sh
clanker --mode json -p "summarize README.md"
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

- Other vendors' native protocols (Anthropic Messages, Google Gemini) and the
  subscription OAuth flows that depend on them.
- A bidirectional RPC mode. `-p` with `--mode text|json` is the non-interactive
  surface; there is no stdin-driven protocol yet.
- TUI niceties: visual-row (vs logical-line) Up/Down, syntax highlighting,
  images, undo. The full-screen differential TUI itself is done.
- Skills packages (`pi install`-style distribution), general extensions, and
  additional providers. Local skill and prompt-template discovery is done.
- pi's two-phase split-turn summary for a single turn that exceeds the
  retained-tail budget. One-shot `/compact <instructions>` is done.
- Fuzzy/regex session search, generated names, pricing, deletion UX, and a
  derived session metadata sidecar.

## License

[MIT](LICENSE), matching the license already declared for this project.
Upstream material retains its own license and copyright; see
[third-party notices](THIRD_PARTY_NOTICES.md), including pi and the adapted
jcode benchmark harness. CLI archives and installed SDKs include these notices.
