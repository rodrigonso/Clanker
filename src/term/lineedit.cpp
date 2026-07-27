#include "term/lineedit.h"

#include <string.h>

#include "term/cell.h"

namespace {

const u32 NO_SELECTION = 0xFFFFFFFFu;

void TextInsert(StrBuf* b, u32 at, StrView s) {
  if (s.Length == 0) return;
  StrBuf_Ensure(b, s.Length);
  memmove(b->Data + at + s.Length, b->Data + at, b->Length - at);
  memcpy(b->Data + at, s.Data, s.Length);
  b->Length += s.Length;
  b->Data[b->Length] = '\0';
}

void TextDelete(StrBuf* b, u32 at, u32 n) {
  if (n == 0) return;
  memmove(b->Data + at, b->Data + at + n, b->Length - (at + n));
  b->Length -= n;
  b->Data[b->Length] = '\0';
}

u32 PrevCharStart(StrView t, u32 off) {
  if (off == 0) return 0;
  u32 i = off - 1;
  while (i > 0 && ((unsigned char)t.Data[i] & 0xC0) == 0x80) i--;
  return i;
}

u32 NextCharEnd(StrView t, u32 off) {
  if (off >= t.Length) return t.Length;
  u32 i = off + 1;
  while (i < t.Length && ((unsigned char)t.Data[i] & 0xC0) == 0x80) i++;
  return i;
}

u32 ClampCharBoundary(StrView t, u32 off) {
  if (off > t.Length) off = t.Length;
  while (off > 0 && off < t.Length && ((unsigned char)t.Data[off] & 0xC0) == 0x80) off--;
  return off;
}

u32 LineStartOf(StrView t, u32 off) {
  u32 i = off;
  while (i > 0 && t.Data[i - 1] != '\n') i--;
  return i;
}

u32 LineEndOf(StrView t, u32 off) {
  u32 i = off;
  while (i < t.Length && t.Data[i] != '\n') i++;
  return i;
}

u32 DisplayCharWidth(u32 cp) {
  u32 width = CharWidth(cp);
  return width < 1 ? 1 : width;
}

u32 ColOfOffset(StrView t, u32 off) {
  u32 ls = LineStartOf(t, off);
  u32 i = ls;
  u32 col = 0;
  while (i < off) col += DisplayCharWidth(Utf8Decode(t, &i));
  return col;
}

u32 OffsetForColumn(StrView t, u32 start, u32 end, u32 col, bool nearest) {
  u32 i = start;
  u32 w = 0;
  while (i < end) {
    u32 next = i;
    u32 cp = Utf8Decode(t, &next);
    u32 cw = DisplayCharWidth(cp);
    if (w + cw > col) return nearest && (col - w) * 2 >= cw ? next : i;
    w += cw;
    i = next;
  }
  return i;
}

bool IsWordByte(char c) { return c != ' ' && c != '\t' && c != '\n'; }

}  // namespace

void LineEdit_Init(LineEdit* e, Arena* mem) {
  e->Mem = mem;
  e->Text = StrBuf_Make(mem, 128);
  e->Cursor = 0;
  e->Anchor = NO_SELECTION;
  e->DesiredCol = 0;
  e->History = Array_Make<StrView>(mem, 16);
  e->HistPos = -1;
  e->Stash = StrBuf_Make(mem, 128);
}

void LineEdit_Clear(LineEdit* e) {
  StrBuf_Clear(&e->Text);
  e->Cursor = 0;
  e->Anchor = NO_SELECTION;
  e->DesiredCol = 0;
  e->HistPos = -1;
}

StrView LineEdit_Text(const LineEdit* e) { return StrView{e->Text.Data, e->Text.Length}; }
u32 LineEdit_Cursor(const LineEdit* e) { return e->Cursor; }
bool LineEdit_Empty(const LineEdit* e) { return e->Text.Length == 0; }
bool LineEdit_HasSelection(const LineEdit* e) {
  return e->Anchor != NO_SELECTION && e->Anchor != e->Cursor;
}
bool LineEdit_SelectionBounds(const LineEdit* e, u32* start, u32* end) {
  u32 a = e->Anchor != NO_SELECTION ? e->Anchor : e->Cursor;
  *start = a < e->Cursor ? a : e->Cursor;
  *end = a < e->Cursor ? e->Cursor : a;
  return *start != *end;
}
StrView LineEdit_SelectedText(const LineEdit* e) {
  u32 start, end;
  LineEdit_SelectionBounds(e, &start, &end);
  return StrView{e->Text.Data + start, end - start};
}
void LineEdit_ClearSelection(LineEdit* e) {
  e->Anchor = NO_SELECTION;
}

static void SyncDesired(LineEdit* e) {
  e->DesiredCol = (u16)ColOfOffset(LineEdit_Text(e), e->Cursor);
}

static bool DeleteSelection(LineEdit* e) {
  u32 start, end;
  if (!LineEdit_SelectionBounds(e, &start, &end)) {
    LineEdit_ClearSelection(e);
    return false;
  }
  TextDelete(&e->Text, start, end - start);
  e->Cursor = start;
  LineEdit_ClearSelection(e);
  SyncDesired(e);
  return true;
}

static bool PrepareMotion(LineEdit* e, bool extend, bool towardEnd) {
  if (extend) {
    if (e->Anchor == NO_SELECTION) e->Anchor = e->Cursor;
    return true;
  }
  if (LineEdit_HasSelection(e)) {
    u32 start, end;
    LineEdit_SelectionBounds(e, &start, &end);
    e->Cursor = towardEnd ? end : start;
    LineEdit_ClearSelection(e);
    SyncDesired(e);
    return false;
  }
  LineEdit_ClearSelection(e);
  return true;
}

void LineEdit_SetCursor(LineEdit* e, u32 offset, bool extend) {
  if (extend && e->Anchor == NO_SELECTION) e->Anchor = e->Cursor;
  e->Cursor = ClampCharBoundary(LineEdit_Text(e), offset);
  if (!extend) LineEdit_ClearSelection(e);
  SyncDesired(e);
}

u32 LineEdit_HitTest(const LineEdit* e, u16 wrapWidth, u32 row, u32 col) {
  StrView t = LineEdit_Text(e);
  u32 width = wrapWidth > 0 ? wrapWidth : 1;
  u32 displayRow = 0;
  u32 rowStart = 0;
  u32 displayCol = 0;
  u32 i = 0;
  while (i < t.Length) {
    if (t.Data[i] == '\n') {
      if (displayRow == row) return OffsetForColumn(t, rowStart, i, col, true);
      displayRow++;
      i++;
      rowStart = i;
      displayCol = 0;
      continue;
    }
    u32 next = i;
    u32 cp = Utf8Decode(t, &next);
    u32 cw = DisplayCharWidth(cp);
    if (displayCol + cw > width) {
      if (displayRow == row) return OffsetForColumn(t, rowStart, i, col, true);
      displayRow++;
      rowStart = i;
      displayCol = 0;
    }
    displayCol += cw;
    i = next;
  }
  if (displayRow == row) return OffsetForColumn(t, rowStart, t.Length, col, true);
  return t.Length;
}

void LineEdit_SetCursorAt(LineEdit* e, u16 wrapWidth, u32 row, u32 col, bool extend) {
  LineEdit_SetCursor(e, LineEdit_HitTest(e, wrapWidth, row, col), extend);
}

void LineEdit_InsertText(LineEdit* e, StrView s) {
  DeleteSelection(e);
  TextInsert(&e->Text, e->Cursor, s);
  e->Cursor += s.Length;
  SyncDesired(e);
}

void LineEdit_InsertChar(LineEdit* e, u32 cp) {
  char buf[4];
  u32 n = 0;
  if (cp < 0x80) {
    buf[n++] = (char)cp;
  } else if (cp < 0x800) {
    buf[n++] = (char)(0xC0 | (cp >> 6));
    buf[n++] = (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    buf[n++] = (char)(0xE0 | (cp >> 12));
    buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[n++] = (char)(0x80 | (cp & 0x3F));
  } else {
    buf[n++] = (char)(0xF0 | (cp >> 18));
    buf[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
    buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[n++] = (char)(0x80 | (cp & 0x3F));
  }
  LineEdit_InsertText(e, StrView{buf, n});
}

void LineEdit_Newline(LineEdit* e) { LineEdit_InsertText(e, SVL("\n")); }

void LineEdit_Backspace(LineEdit* e) {
  if (DeleteSelection(e)) return;
  if (e->Cursor == 0) return;
  u32 ps = PrevCharStart(LineEdit_Text(e), e->Cursor);
  TextDelete(&e->Text, ps, e->Cursor - ps);
  e->Cursor = ps;
  SyncDesired(e);
}

void LineEdit_Delete(LineEdit* e) {
  if (DeleteSelection(e)) return;
  if (e->Cursor >= e->Text.Length) return;
  u32 ne = NextCharEnd(LineEdit_Text(e), e->Cursor);
  TextDelete(&e->Text, e->Cursor, ne - e->Cursor);
}

void LineEdit_Left(LineEdit* e, bool extend) {
  if (!PrepareMotion(e, extend, false)) return;
  if (e->Cursor > 0) e->Cursor = PrevCharStart(LineEdit_Text(e), e->Cursor);
  SyncDesired(e);
}

void LineEdit_Right(LineEdit* e, bool extend) {
  if (!PrepareMotion(e, extend, true)) return;
  if (e->Cursor < e->Text.Length) e->Cursor = NextCharEnd(LineEdit_Text(e), e->Cursor);
  SyncDesired(e);
}

void LineEdit_Home(LineEdit* e, bool extend) {
  if (!PrepareMotion(e, extend, false)) return;
  e->Cursor = LineStartOf(LineEdit_Text(e), e->Cursor);
  e->DesiredCol = 0;
}

void LineEdit_End(LineEdit* e, bool extend) {
  if (!PrepareMotion(e, extend, true)) return;
  e->Cursor = LineEndOf(LineEdit_Text(e), e->Cursor);
  SyncDesired(e);
}

bool LineEdit_Up(LineEdit* e, bool extend) {
  if (!PrepareMotion(e, extend, false)) return true;
  StrView t = LineEdit_Text(e);
  u32 ls = LineStartOf(t, e->Cursor);
  if (ls == 0) return false;
  u32 prevStart = LineStartOf(t, ls - 1);
  u32 prevEnd = ls - 1;
  e->Cursor = OffsetForColumn(t, prevStart, prevEnd, e->DesiredCol, false);
  return true;
}

bool LineEdit_Down(LineEdit* e, bool extend) {
  if (!PrepareMotion(e, extend, true)) return true;
  StrView t = LineEdit_Text(e);
  u32 le = LineEndOf(t, e->Cursor);
  if (le >= t.Length) return false;
  u32 nextStart = le + 1;
  u32 nextEnd = LineEndOf(t, nextStart);
  e->Cursor = OffsetForColumn(t, nextStart, nextEnd, e->DesiredCol, false);
  return true;
}

void LineEdit_WordLeft(LineEdit* e, bool extend) {
  if (!PrepareMotion(e, extend, false)) return;
  StrView t = LineEdit_Text(e);
  u32 i = e->Cursor;
  while (i > 0 && !IsWordByte(t.Data[i - 1])) i--;
  while (i > 0 && IsWordByte(t.Data[i - 1])) i--;
  e->Cursor = i;
  SyncDesired(e);
}

void LineEdit_WordRight(LineEdit* e, bool extend) {
  if (!PrepareMotion(e, extend, true)) return;
  StrView t = LineEdit_Text(e);
  u32 i = e->Cursor;
  while (i < t.Length && !IsWordByte(t.Data[i])) i++;
  while (i < t.Length && IsWordByte(t.Data[i])) i++;
  e->Cursor = i;
  SyncDesired(e);
}

void LineEdit_DeleteWordBack(LineEdit* e) {
  if (DeleteSelection(e)) return;
  StrView t = LineEdit_Text(e);
  u32 i = e->Cursor;
  while (i > 0 && !IsWordByte(t.Data[i - 1])) i--;
  while (i > 0 && IsWordByte(t.Data[i - 1])) i--;
  if (i < e->Cursor) {
    TextDelete(&e->Text, i, e->Cursor - i);
    e->Cursor = i;
  }
  SyncDesired(e);
}

void LineEdit_KillToEnd(LineEdit* e) {
  if (DeleteSelection(e)) return;
  u32 le = LineEndOf(LineEdit_Text(e), e->Cursor);
  TextDelete(&e->Text, e->Cursor, le - e->Cursor);
}

void LineEdit_KillToStart(LineEdit* e) {
  if (DeleteSelection(e)) return;
  u32 ls = LineStartOf(LineEdit_Text(e), e->Cursor);
  TextDelete(&e->Text, ls, e->Cursor - ls);
  e->Cursor = ls;
  SyncDesired(e);
}

bool LineEdit_OnFirstLine(const LineEdit* e) {
  return LineStartOf(LineEdit_Text(e), e->Cursor) == 0;
}
bool LineEdit_OnLastLine(const LineEdit* e) {
  return LineEndOf(LineEdit_Text(e), e->Cursor) >= e->Text.Length;
}

static void LoadInto(LineEdit* e, StrView s) {
  StrBuf_Clear(&e->Text);
  StrBuf_Append(&e->Text, s);
  e->Cursor = e->Text.Length;
  LineEdit_ClearSelection(e);
  SyncDesired(e);
}

void LineEdit_HistoryPrev(LineEdit* e) {
  if (e->History.Count == 0) return;
  if (e->HistPos < 0) {
    StrBuf_Clear(&e->Stash);
    StrBuf_Append(&e->Stash, LineEdit_Text(e));
    e->HistPos = (i32)e->History.Count - 1;
  } else if (e->HistPos > 0) {
    e->HistPos--;
  } else {
    return;
  }
  LoadInto(e, e->History.Data[e->HistPos]);
}

void LineEdit_HistoryNext(LineEdit* e) {
  if (e->HistPos < 0) return;
  e->HistPos++;
  if (e->HistPos >= (i32)e->History.Count) {
    e->HistPos = -1;
    LoadInto(e, StrView{e->Stash.Data, e->Stash.Length});
  } else {
    LoadInto(e, e->History.Data[e->HistPos]);
  }
}

StrView LineEdit_Submit(LineEdit* e) {
  StrView text = StrView_Dup(e->Mem, LineEdit_Text(e));
  if (text.Length) Array_Push(&e->History, text);
  LineEdit_Clear(e);
  return text;
}
