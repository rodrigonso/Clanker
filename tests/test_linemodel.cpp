#include "base/arena.h"
#include "check.h"
#include "term/linemodel.h"

namespace {

bool RowHas(Arena* a, const SpanLine* sl, const char* needle) {
  StrBuf b = StrBuf_Make(a, 64);
  for (u32 i = 0; i < sl->Spans.Count; i++) StrBuf_Append(&b, sl->Spans.Data[i].Text);
  return StrView_Contains(StrBuf_View(&b), needle);
}

u16 RowWidth(const SpanLine* sl) {
  u16 w = 0;
  for (u32 i = 0; i < sl->Spans.Count; i++) w = (u16)(w + TextWidth(sl->Spans.Data[i].Text));
  return w;
}

bool AnyRowHas(Arena* a, LineModel* m, const char* needle) {
  for (u32 i = 0; i < LineModel_Count(m); i++)
    if (RowHas(a, LineModel_At(m, i), needle)) return true;
  return false;
}

}  // namespace

void TestLineModel() {
  Arena a = ArenaCreate();
  LineModel_SetColorEnabled(true);

  LineModel m;
  LineModel_Init(&m, &a, 20, 5000);

  // A user entry: label row + content row.
  LineModel_AddSimple(&m, EntryKind::User, StrView{nullptr, 0}, SVL("hello world"));
  CHECK(LineModel_Count(&m) >= 2);
  CHECK(AnyRowHas(&a, &m, "you"));
  CHECK(AnyRowHas(&a, &m, "hello world"));

  // Wrapping: a long line must split into rows no wider than the width.
  LineModel_AddSimple(&m, EntryKind::Info, StrView{nullptr, 0},
                      SVL("the quick brown fox jumps over the lazy dog again and again"));
  for (u32 i = 0; i < LineModel_Count(&m); i++) CHECK(RowWidth(LineModel_At(&m, i)) <= 20);

  // Streaming assistant with markdown; re-render across appends.
  u32 before = LineModel_Count(&m);
  LineModel_Begin(&m, EntryKind::Assistant, StrView{nullptr, 0}, true);
  LineModel_Append(&m, SVL("Hello "));
  LineModel_Append(&m, SVL("**world**"));
  LineModel_End(&m);
  CHECK(LineModel_Count(&m) > before);
  CHECK(AnyRowHas(&a, &m, "assistant"));
  CHECK(AnyRowHas(&a, &m, "world"));

  // A tool call entry shows the gear glyph + name.
  LineModel_AddSimple(&m, EntryKind::ToolCall, SVL("bash"), SVL("{\"command\":\"ls\"}"));
  CHECK(AnyRowHas(&a, &m, "bash"));

  // Re-wrap narrower: all rows respect the new width.
  LineModel_SetWidth(&m, 10);
  for (u32 i = 0; i < LineModel_Count(&m); i++) CHECK(RowWidth(LineModel_At(&m, i)) <= 10);
  CHECK(AnyRowHas(&a, &m, "world"));

  ArenaRelease(&a);
}
