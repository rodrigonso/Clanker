#pragma once
#include "base/arena.h"
#include "base/basics.h"
#include "base/str.h"

enum class ConfigFileReadResult : u8 {
  Success,
  Missing,
  Failure,
};

// Reads the complete file into `arena`. Successful content is null-terminated
// at [Length] for parsers that require it.
ConfigFileReadResult ConfigFile_ReadAll(Arena* arena, StrView path, StrView* out);

// Creates the immediate parent directory when needed. Existing parents succeed.
bool ConfigFile_EnsureParentDir(Arena* arena, StrView path);

// Replaces `path` via a sibling ".tmp" file. Resulting POSIX files are mode 0600.
bool ConfigFile_WriteAtomic(Arena* arena, StrView path, StrView content);
