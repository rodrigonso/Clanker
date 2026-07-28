#pragma once
#include "base/str.h"

enum class ToolViewKind : u8 { Unknown, Shell, Read, Grep, Find, Ls, Write, Edit };

struct ToolView {
  ToolViewKind Kind;
  StrView Marker;
  StrView Label;
  StrBuf Detail;
  StrBuf Outcome;
  // Byte offset into Outcome where the "removed" half of a diff stat begins, so
  // the renderer can colour the two halves differently. 0 means the outcome is a
  // single uniformly styled run.
  u32 OutcomeSplit;
  u32 CountHint;
};

void ToolView_Init(Arena* arena, ToolView* view, StrView name, StrView argsJson);
void ToolView_Finish(ToolView* view, StrView result, bool isError);
