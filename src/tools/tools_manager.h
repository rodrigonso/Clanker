#pragma once
#include "base/arena.h"
#include "base/str.h"

// Returns a local ripgrep path or the PATH command name. If ripgrep is absent,
// downloads the current release into ~/.clanker/bin, matching pi's tool policy.
StrView ToolManager_EnsureRipgrep(Arena* arena, volatile i32* abort, StrBuf* err);
