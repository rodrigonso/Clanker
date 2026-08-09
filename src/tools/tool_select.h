#pragma once
#include "agent/tool.h"
#include "base/arena.h"
#include "base/array.h"
#include "base/str.h"

struct ToolSelection {
  // Empty means every tool.
  const StrView* Allow;
  u32 AllowCount;
  const StrView* Exclude;
  u32 ExcludeCount;
  bool NoTools;
};

// Appends trimmed, non-empty comma-separated names to `out`.
void ToolSelect_SplitNames(Arena* arena, StrView value, Array<StrView>* out);

// Collects unique selected names absent from `tools`.
void ToolSelect_UnknownNames(Arena* arena, const Tool* tools, u32 count,
                             const ToolSelection* selection,
                             Array<StrView>* out);

// `out` must hold `count` tools. Exclusion wins over the allowlist.
u32 ToolSelect_Apply(const Tool* tools, u32 count,
                     const ToolSelection* selection, Tool* out);
