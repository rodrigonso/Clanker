#include "tools/tool_select.h"

namespace {

bool Contains(const StrView* names, u32 count, StrView name) {
  for (u32 i = 0; i < count; i++)
    if (StrView_Eq(names[i], name)) return true;
  return false;
}

bool IsToolName(const Tool* tools, u32 count, StrView name) {
  for (u32 i = 0; i < count; i++)
    if (StrView_Eq(tools[i].Name, name)) return true;
  return false;
}

void CollectUnknown(Arena* arena, const Tool* tools, u32 count,
                    const StrView* names, u32 nameCount, Array<StrView>* out) {
  for (u32 i = 0; i < nameCount; i++) {
    if (IsToolName(tools, count, names[i])) continue;
    if (Contains(out->Data, out->Count, names[i])) continue;
    if (!out->Capacity) *out = Array_Make<StrView>(arena, 4);
    Array_Push(out, names[i]);
  }
}

}  // namespace

void ToolSelect_SplitNames(Arena* arena, StrView value, Array<StrView>* out) {
  u32 start = 0;
  for (u32 i = 0; i <= value.Length; i++) {
    if (i != value.Length && value.Data[i] != ',') continue;
    StrView piece =
        StrView_Trim(StrView{value.Data + start, i - start});
    if (piece.Length) {
      if (!out->Capacity) *out = Array_Make<StrView>(arena, 8);
      Array_Push(out, piece);
    }
    start = i + 1;
  }
}

void ToolSelect_UnknownNames(Arena* arena, const Tool* tools, u32 count,
                             const ToolSelection* selection,
                             Array<StrView>* out) {
  if (!selection) return;
  CollectUnknown(arena, tools, count, selection->Allow, selection->AllowCount,
                 out);
  CollectUnknown(arena, tools, count, selection->Exclude,
                 selection->ExcludeCount, out);
}

u32 ToolSelect_Apply(const Tool* tools, u32 count,
                     const ToolSelection* selection, Tool* out) {
  if (!tools || !out) return 0;
  if (!selection) {
    for (u32 i = 0; i < count; i++) out[i] = tools[i];
    return count;
  }
  if (selection->NoTools) return 0;

  u32 written = 0;
  for (u32 i = 0; i < count; i++) {
    StrView name = tools[i].Name;
    if (selection->AllowCount &&
        !Contains(selection->Allow, selection->AllowCount, name))
      continue;
    if (Contains(selection->Exclude, selection->ExcludeCount, name)) continue;
    out[written++] = tools[i];
  }
  return written;
}
