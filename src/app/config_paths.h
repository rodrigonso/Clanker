#pragma once

#include "base/str.h"

// Every per-user config and data file lives under one directory. Building that
// prefix in each module drifted: three call sites fell back to "." when the
// home directory was unknown and one did not, which produced an absolute
// "/.clanker" instead of a relative path.

// <home>/.clanker, or ./.clanker when the home directory is unknown.
StrView ClankerDataRoot(Arena* arena);

// <home>/.clanker/<name>, with the same fallback.
StrView ClankerDataPath(Arena* arena, const char* name);
