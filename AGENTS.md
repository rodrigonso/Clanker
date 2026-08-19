# Clank agent rules

A C++17 reimplementation of the core of [`earendil-works/pi`](https://github.com/earendil-works/pi)
without a JavaScript runtime. Footprint is the point: prefer the change that
allocates less and copies less.

## Commands

Build and test through the repo scripts rather than invoking CMake directly:

```powershell
.\build.ps1                                  # build clank (Release)
.\build.ps1 -Config Debug                    # debug build
.\build.ps1 -Test -TestArgs session,tools    # build + run named suites
.\build.ps1 -Test                            # full offline sweep
.\run.ps1 -- -p "summarize README.md"        # build, then run; Clank flags after --
```

Suites are named in `tests/test_main.cpp` (`session`, `harness`, `compaction`,
`tools`, `infra`, `prompt_context`, ...). Run the narrowest suite that covers
your change; use `.\build.ps1 -Test` before declaring a change finished.
Network-tagged suites only run with `--all` and are not part of a normal pass.

For ad-hoc scripts, `write` them to a temp file (e.g. the OS temp directory or
`/tmp`), run them, edit if needed, and remove them when done. Do not embed
multi-line scripts in `shell` commands, and do not leave scratch scripts,
scratch notes, or generated sample data in the repository tree. Nothing outside
`build/`, `build-rel/`, and `out/` is expected to be disposable, and those are
already ignored.

Never commit anything under `~/.clank` or a session log; those are user data.

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

The provider layer is a deliberate divergence: Clank speaks only
OpenAI-compatible Chat Completions (`src/llm/chat_completions*`), not pi's
vendor-native protocols. A provider is a row in
`src/app/provider_registry.cpp` pairing a `ProviderSpec` with a `ChatEndpoint`;
adding a vendor means adding a row, not writing wire code. Thinking blocks are
never sent back and `reasoning_effort` is clamped to `low`/`medium`/`high`,
because the portable API defines nothing richer.

## Sessions and files on disk

- Session logs: `~/.clank/sessions/v2/<project-key>/<session-id>.jsonl`, with a
  `<session-id>.lock` sentinel that exists only while a writer holds it and a
  `<session-id>/` directory for that session's other artifacts.
- Config: `~/.clank/settings.json`, `auth.json`, `trust.json`, and
  `.clank/hooks.json` per project. All config writes go through
  `ConfigFile_WriteAtomic`, which writes a sibling temp file and removes it on
  every failure path -- use it instead of writing config in place.
- The `shell` tool spills output over the truncation limit to a randomly named
  file in the session's artifact directory (`shell-<hex>.log`) and reports the
  path in the tool result. That file is intentionally left for the user to
  read. Under `--no-session` it falls back to the OS temp directory.

## Git

Stage explicit paths; never `git add -A` from the repo root. Only commit files
you changed for the task at hand.
