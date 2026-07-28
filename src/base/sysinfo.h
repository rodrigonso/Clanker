#pragma once
#include "base/arena.h"
#include "base/basics.h"
#include "base/str.h"

// Peak resident set size (working set) of the current process, in bytes.
u64 PeakRssBytes();

// Current working directory of the process, copied into `a`, with any trailing
// separator stripped. Empty if it cannot be determined.
StrView Sys_CurrentDir(Arena* a);

// Change the process working directory. Returns false when `path` is empty,
// missing, not a directory, or cannot be entered.
bool Sys_SetCurrentDir(StrView path);

// User home directory from the environment (USERPROFILE on Windows, HOME
// elsewhere), copied into `a` with any trailing separator stripped. Empty if
// unset.
StrView Sys_HomeDir(Arena* a);

// If `path` begins with `home` at a path-separator boundary, return a copy with
// that prefix replaced by "~"; otherwise a plain copy of `path`. On Windows the
// prefix match is case-insensitive and treats '/' and '\\' as equivalent. Pure.
StrView Sys_AbbreviatePath(Arena* a, StrView path, StrView home);

// Current git branch for the repository containing `dir`, copied into `a`, or
// empty if `dir` is not inside a git work tree or HEAD is detached. Walks up the
// directory chain and follows the `.git` file (worktree/submodule) indirection.
StrView Sys_GitBranch(Arena* a, StrView dir);

// Pure parsers (exposed for testing).
StrView Git_GitdirFromFile(StrView contents);  // "gitdir: <path>"      -> <path>
StrView Git_BranchFromHead(StrView contents);  // "ref: refs/heads/<b>" -> <b>
