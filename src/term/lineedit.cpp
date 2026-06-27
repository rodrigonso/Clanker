#include "term/lineedit.h"

#include <string.h>

#include "term/cell.h"

namespace {

// Mid-buffer insert/delete on a StrBuf (kept null-terminated).
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

u32 ColOfOffset(StrView t, u32 off) {
  u32 ls = LineStartOf(t, off);
  return TextWidth(StrView{t.Data + ls, off - ls});
}

// Byte offset within [lineStart, lineEnd] whose display column best matches col.
u32 OffsetForCol(StrView t, u32 lineStart, u32 lineEnd, u32 col) {
  u32 i = lineStart;
  u32 w = 0;
  while (i < lineEnd) {
    u32 jj = i;
    u32 cp = Utf8Decode(StrView{t.Data, t.Length}, &jj);
    u32 cw = CharWidth(cp);
    if (w + cw > col) break;
    w += cw;
    i = jj;
  }
  return i;
}

bool IsWordByte(char c) { return c != ' ' && c != '\t' && c != '\n'; }

}  // namespace

void LineEdit_Init(LineEdit* e, Arena* mem) {
  e->Mem = mem;
  e->Text = StrBuf_Make(mem, 128);
  e->Cursor = 0;
  e->DesiredCol = 0;
  e->History = Array_Make<StrView>(mem, 16);
  e->HistPos = -1;
  e->Stash = StrBuf_Make(mem, 128);
  e->StashValid = false;
}

void LineEdit_Clear(LineEdit* e) {
  StrBuf_Clear(&e->Text);
  e->Cursor = 0;
  e->DesiredCol = 0;
  e->HistPos = -1;
  e->StashValid = false;
}

StrView LineEdit_Text(const LineEdit* e) { return StrView{e->Text.Data, e->Text.Length}; }
u32 LineEdit_Cursor(const LineEdit* e) { return e->Cursor; }
bool LineEdit_Empty(const LineEdit* e) { return e->Text.Length == 0; }

static void SyncDesired(LineEdit* e) {
  e->DesiredCol = (u16)ColOfOffset(LineEdit_Text(e), e->Cursor);
}

void LineEdit_InsertText(LineEdit* e, StrView s) {
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
  if (e->Cursor == 0) return;
  u32 ps = PrevCharStart(LineEdit_Text(e), e->Cursor);
  TextDelete(&e->Text, ps, e->Cursor - ps);
  e->Cursor = ps;
  SyncDesired(e);
}

void LineEdit_Delete(LineEdit* e) {
  if (e->Cursor >= e->Text.Length) return;
  u32 ne = NextCharEnd(LineEdit_Text(e), e->Cursor);
  TextDelete(&e->Text, e->Cursor, ne - e->Cursor);
}

void LineEdit_Left(LineEdit* e) {
  if (e->Cursor > 0) e->Cursor = PrevCharStart(LineEdit_Text(e), e->Cursor);
  SyncDesired(e);
}

void LineEdit_Right(LineEdit* e) {
  if (e->Cursor < e->Text.Length) e->Cursor = NextCharEnd(LineEdit_Text(e), e->Cursor);
  SyncDesired(e);
}

void LineEdit_Home(LineEdit* e) {
  e->Cursor = LineStartOf(LineEdit_Text(e), e->Cursor);
  e->DesiredCol = 0;
}

void LineEdit_End(LineEdit* e) {
  e->Cursor = LineEndOf(LineEdit_Text(e), e->Cursor);
  SyncDesired(e);
}

bool LineEdit_Up(LineEdit* e) {
  StrView t = LineEdit_Text(e);
  u32 ls = LineStartOf(t, e->Cursor);
  if (ls == 0) return false;
  u32 prevStart = LineStartOf(t, ls - 1);
  u32 prevEnd = ls - 1;
  e->Cursor = OffsetForCol(t, prevStart, prevEnd, e->DesiredCol);
  return true;
}

bool LineEdit_Down(LineEdit* e) {
  StrView t = LineEdit_Text(e);
  u32 le = LineEndOf(t, e->Cursor);
  if (le >= t.Length) return false;
  u32 nextStart = le + 1;
  u32 nextEnd = LineEndOf(t, nextStart);
  e->Cursor = OffsetForCol(t, nextStart, nextEnd, e->DesiredCol);
  return true;
}

void LineEdit_WordLeft(LineEdit* e) {
  StrView t = LineEdit_Text(e);
  u32 i = e->Cursor;
  while (i > 0 && !IsWordByte(t.Data[i - 1])) i--;
  while (i > 0 && IsWordByte(t.Data[i - 1])) i--;
  e->Cursor = i;
  SyncDesired(e);
}

void LineEdit_WordRight(LineEdit* e) {
  StrView t = LineEdit_Text(e);
  u32 i = e->Cursor;
  while (i < t.Length && !IsWordByte(t.Data[i])) i++;
  while (i < t.Length && IsWordByte(t.Data[i])) i++;
  e->Cursor = i;
  SyncDesired(e);
}

void LineEdit_DeleteWordBack(LineEdit* e) {
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
  u32 le = LineEndOf(LineEdit_Text(e), e->Cursor);
  TextDelete(&e->Text, e->Cursor, le - e->Cursor);
}

void LineEdit_KillToStart(LineEdit* e) {
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
  SyncDesired(e);
}

void LineEdit_HistoryPrev(LineEdit* e) {
  if (e->History.Count == 0) return;
  if (e->HistPos < 0) {
    StrBuf_Clear(&e->Stash);
    StrBuf_Append(&e->Stash, LineEdit_Text(e));
    e->StashValid = true;
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
    LoadInto(e, e->StashValid ? StrView{e->Stash.Data, e->Stash.Length} : StrView{"", 0});
    e->StashValid = false;
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
