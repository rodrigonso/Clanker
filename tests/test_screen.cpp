#include "base/arena.h"
#include "base/str.h"
#include "check.h"
#include "term/screen.h"

namespace {

struct Cap {
  StrBuf B;
};

void CapWrite(void* u, const char* d, u32 n) { StrBuf_Append(&((Cap*)u)->B, StrView{d, n}); }

}  // namespace

void TestScreen() {
  Arena a = ArenaCreate();
  Cap cap;
  cap.B = StrBuf_Make(&a, 256);

  Screen s;
  Screen_Init(&s, &a, CapWrite, &cap);
  Screen_Resize(&s, 10, 3);
  Screen_SetCursor(&s, false, 0, 0);  // keep cursor control out of the diff assertions

  // Frame 1: paint "hi".
  Screen_BeginFrame(&s);
  Screen_PutText(&s, 0, 0, SVL("hi"), StyleDefault(), 10);
  CHECK(Screen_BackAt(&s, 0, 0).Ch == 'h');
  CHECK(Screen_BackAt(&s, 1, 0).Ch == 'i');
  StrBuf_Clear(&cap.B);
  Screen_Flush(&s);
  CHECK(Screen_FrontAt(&s, 0, 0).Ch == 'h');
  CHECK(Screen_FrontAt(&s, 1, 0).Ch == 'i');
  CHECK(StrView_Contains(StrBuf_View(&cap.B), "hi"));

  // Frame 2: change only column 1 ('i' -> 'o'); diff must not rewrite 'h'.
  Screen_BeginFrame(&s);
  Screen_PutText(&s, 0, 0, SVL("ho"), StyleDefault(), 10);
  StrBuf_Clear(&cap.B);
  Screen_Flush(&s);
  StrView out2 = StrBuf_View(&cap.B);
  CHECK(StrView_Contains(out2, "o"));
  CHECK(!StrView_Contains(out2, "h"));  // 'h' unchanged -> not repainted (no lowercase h emitted)
  CHECK(Screen_FrontAt(&s, 1, 0).Ch == 'o');

  // Frame 3: identical content -> no glyph output at all.
  Screen_BeginFrame(&s);
  Screen_PutText(&s, 0, 0, SVL("ho"), StyleDefault(), 10);
  StrBuf_Clear(&cap.B);
  Screen_Flush(&s);
  StrView out3 = StrBuf_View(&cap.B);
  CHECK(!StrView_Contains(out3, "o"));

  // Styled run emits an SGR with the palette color.
  Screen_BeginFrame(&s);
  Screen_PutText(&s, 0, 2, SVL("R"), StyleMake(196, COL_DEFAULT, ATTR_BOLD), 10);
  StrBuf_Clear(&cap.B);
  Screen_Flush(&s);
  StrView out4 = StrBuf_View(&cap.B);
  CHECK(StrView_Contains(out4, "38;5;196"));
  CHECK(StrView_Contains(out4, "R"));

  // Wide glyph (U+4F60) occupies two columns; the next is a continuation cell.
  Screen_BeginFrame(&s);
  Screen_PutText(&s, 0, 1, SVL("\xe4\xbd\xa0X"), StyleDefault(), 10);
  CHECK(Screen_BackAt(&s, 0, 1).Ch == 0x4F60);
  CHECK(Screen_BackAt(&s, 1, 1).Ch == CELL_CONT);
  CHECK(Screen_BackAt(&s, 2, 1).Ch == 'X');

  // A narrower resize forces a full repaint flag.
  Screen_Resize(&s, 6, 2);
  CHECK(s.ForceRepaint);

  ArenaRelease(&a);
}
