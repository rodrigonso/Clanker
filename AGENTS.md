# Clanker agent rules

A C++17 reimplementation of the core of [`earendil-works/pi`](https://github.com/earendil-works/pi)
without a JavaScript runtime. Footprint is the point: prefer the change that
allocates less and copies less.

## Commands

Build and test through the repo scripts. They are thin wrappers around CMake:

```powershell
.\build.bat                                 # build clanker (Release)
.\build.bat -Config Debug                   # debug build
.\build.bat -Test -TestArgs session,tools    # build + run named suites
.\build.bat -Test                           # full offline sweep
.\build.bat -CoreOnly -Examples -Test        # no networking dependencies
.\tools\run.ps1 -- -p "summarize README.md"  # build, then run; Clanker flags after --
```

On Linux/macOS use `bash tools/build.sh --test`, or
`bash tools/build.sh --core-only --examples --test` for the core SDK.
Core-only builds default to `out/core`; use distinct build directories when
sharing a checkout between operating systems. `-InstallPrefix` /
`--install-prefix` installs the SDK and enabled CLI; `-Package` / `--package`
creates a CLI archive.

Targets and sources are defined in the root `CMakeLists.txt`; only the pinned
CLI dependency policy lives in `cmake/curl.cmake`. Suites are registered in
`tests/suites.txt` (`session`, `harness`, `compaction`,
`tools`, `infra`, `prompt_context`, ...). Run the narrowest suite that covers
your change; use `.\build.bat -Test` before declaring a change finished.
Network-tagged suites only run with `--all` and are not part of a normal pass.

For ad-hoc scripts, `write` them to a temp file (e.g. the OS temp directory or
`/tmp`), run them, edit if needed, and remove them when done. Do not embed
multi-line scripts in `shell` commands, and do not leave scratch scripts,
scratch notes, or generated sample data in the repository tree. Nothing outside
`build/`, `build-rel/`, and `out/` is expected to be disposable, and those are
already ignored.

Never commit anything under `~/.clanker` or a session log; those are user data.

## Code conventions

- **No C++ standard library**, no exceptions, no RTTI. Only the C runtime and
  platform/libcurl APIs. Builds run with `/GR- /EHs-c- /W4` and must stay
  warning-clean.
- Use the house types: `Arena` for all allocation, `Array<T>`, `StrView` /
  `StrBuf` for UTF-8 text. Fields are PascalCase; `FFI(cond)` is the fastfail.
- Arenas are the memory model. A function that needs temporary space creates a
  scratch arena and releases it on every exit path, including error paths.
  "Scratch" in this codebase means arena memory, not a file on disk.
- Prefer explicit parameters over hidden state. Ownership of a string or array
  is always the arena it was allocated from.
- Comment only what needs clarification -- why a non-obvious invariant holds,
  not what the line does.

## Behavioral parity with pi

Prompt-visible strings (the default system prompt preamble, tool prompt
snippets, guideline bullets, and the `<project_context>` framing) are aligned
byte-for-byte with pi's `buildSystemPrompt`. Do not reword them casually; a
reword is a parity change.

The provider layer is a deliberate divergence: Clanker speaks only OpenAI's
Responses API (`src/llm/responses*`), not pi's per-vendor protocols. A provider
is a row in `src/app/provider_registry.cpp` pairing a `ProviderSpec` with a
`ResponsesEndpoint`; adding a compatible vendor means adding a row, not writing
wire code. Thinking blocks *are* replayed, as opaque encrypted reasoning items
carried in `Content::Signature`, and `reasoning_effort` is passed through at
full fidelity rather than clamped.

## Sessions and files on disk

- Session logs: `~/.clanker/sessions/v2/<project-key>/<session-id>.jsonl`, with a
  `<session-id>.lock` sentinel that exists only while a writer holds it and a
  `<session-id>/` directory for that session's other artifacts.
- Config: `~/.clanker/settings.json`, `auth.json`, `trust.json`, and
  `.clanker/hooks.json` per project. All config writes go through
  `ConfigFile_WriteAtomic`, which writes a sibling temp file and removes it on
  every failure path -- use it instead of writing config in place.
- The `shell` tool spills output over the truncation limit to a randomly named
  file in the session's artifact directory (`shell-<hex>.log`) and reports the
  path in the tool result. That file is intentionally left for the user to
  read. Under `--no-session` it falls back to the OS temp directory.

## Git

Stage explicit paths; never `git add -A` from the repo root. Only commit files
you changed for the task at hand.
