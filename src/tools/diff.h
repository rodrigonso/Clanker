#pragma once
#include "base/arena.h"
#include "base/str.h"

struct DiffResult {
  StrView Display;
  StrView Patch;
  i32 FirstChangedLine;
};

// Display format:
//   <prefix><line><pad> <text>
// Prefix is ' ' for context, '-' for old removed lines, and '+' for new added
// lines. Line numbers are 1-indexed and right-padded to the width of the
// largest old/new line number. Omitted unchanged regions are shown as:
//   <space><line-width spaces> ...
// `context` unchanged lines are shown around each change; passing 0 uses 4.
DiffResult Diff_Make(Arena* arena, StrView path, StrView oldText, StrView newText, u32 context);
