#pragma once

#include "base/str.h"

// Every per-user config and data file lives under one directory. Building that
// prefix in each module drifted: three call sites fell back to "." when the
// home directory was unknown and one did not, which produced an absolute
// "/.clank" instead of a relative path.

// <home>/.clank, or ./.clank when the home directory is unknown.
StrView ClankDataRoot(Arena* arena);

// <home>/.clank/<name>, with the same fallback.
StrView ClankDataPath(Arena* arena, const char* name);
