#pragma once
#include "base/arena.h"
#include "base/str.h"

// Finds rg next to an absolute executable path. Empty if the sibling is absent
// or is a directory. Does not search the working directory or PATH.
StrView ToolManager_FindBundledRipgrep(Arena* arena, StrView executablePath);

// CLANKER_RG_PATH takes precedence over bundled rg, the ~/.clanker/bin cache,
// and PATH, in that order. If absent, downloads the current release unless
// offline, matching pi's tool policy.
StrView ToolManager_EnsureRipgrep(Arena* arena, volatile i32* abort, StrBuf* err);
