#pragma once
#include "base/str.h"

enum class ToolViewKind : u8 { Unknown, Bash, Read, Grep, Find, Ls, Write, Edit };

// App-specific presentation model for tool calls.
struct ToolView {
  ToolViewKind Kind;
  StrView Marker;
  StrView Label;
  StrBuf Detail;
  StrBuf Outcome;
  u32 CountHint;
};

void ToolView_Init(Arena* arena, ToolView* view, StrView name, StrView argsJson);
void ToolView_Finish(ToolView* view, StrView result, bool isError);
