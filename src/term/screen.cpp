#include "term/screen.h"

#include <string.h>

namespace {

void EmitUint(StrBuf* b, u32 v) { StrBuf_AppendU64(b, v); }

void EmitCup(StrBuf* b, u16 x, u16 y) {  // 1-based row;col
  StrBuf_AppendCStr(b, "\x1b[");
  EmitUint(b, (u32)y + 1);
  StrBuf_AppendChar(b, ';');
  EmitUint(b, (u32)x + 1);
  StrBuf_AppendChar(b, 'H');
}

void EmitSgr(StrBuf* b, Style st) {
  StrBuf_AppendCStr(b, "\x1b[0");
  if (st.Attr & ATTR_BOLD) StrBuf_AppendCStr(b, ";1");
  if (st.Attr & ATTR_DIM) StrBuf_AppendCStr(b, ";2");
  if (st.Attr & ATTR_ITALIC) StrBuf_AppendCStr(b, ";3");
  if (st.Attr & ATTR_UNDERLINE) StrBuf_AppendCStr(b, ";4");
  if (st.Attr & ATTR_REVERSE) StrBuf_AppendCStr(b, ";7");
  if (st.Fg != COL_DEFAULT) {
    StrBuf_AppendCStr(b, ";38;5;");
    EmitUint(b, st.Fg);
  }
  if (st.Bg != COL_DEFAULT) {
    StrBuf_AppendCStr(b, ";48;5;");
    EmitUint(b, st.Bg);
  }
  StrBuf_AppendChar(b, 'm');
}

void EmitUtf8(StrBuf* b, u32 cp) {
  if (cp == 0) {
    StrBuf_AppendChar(b, ' ');
    return;
  }
  if (cp < 0x80) {
    StrBuf_AppendChar(b, (char)cp);
  } else if (cp < 0x800) {
    StrBuf_AppendChar(b, (char)(0xC0 | (cp >> 6)));
    StrBuf_AppendChar(b, (char)(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    StrBuf_AppendChar(b, (char)(0xE0 | (cp >> 12)));
    StrBuf_AppendChar(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
    StrBuf_AppendChar(b, (char)(0x80 | (cp & 0x3F)));
  } else {
    StrBuf_AppendChar(b, (char)(0xF0 | (cp >> 18)));
    StrBuf_AppendChar(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
    StrBuf_AppendChar(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
    StrBuf_AppendChar(b, (char)(0x80 | (cp & 0x3F)));
  }
}

// Decode one scalar from `s` at *i (advancing it). Returns 0 at end.
u32 Utf8Next(StrView s, u32* i) { return Utf8Decode(s, i); }

}  // namespace

void Screen_Init(Screen* s, Arena* mem, ScreenWriter w, void* user) {
  memset(s, 0, sizeof(*s));
  s->Mem = mem;
  s->Write = w;
  s->WriteUser = user;
  s->CursorVisible = true;
}

void Screen_Resize(Screen* s, u16 cols, u16 rows) {
  if (cols == 0) cols = 1;
  if (rows == 0) rows = 1;
  if (s->Front && s->Back && s->Cols == cols && s->Rows == rows) return;
  u32 need = (u32)cols * rows;
  if (need > s->Cap || !s->Front) {
    s->Front = (Cell*)ArenaAlloc(s->Mem, need * sizeof(Cell), alignof(Cell));
    s->Back = (Cell*)ArenaAlloc(s->Mem, need * sizeof(Cell), alignof(Cell));
    s->Cap = need;
  }
  s->Cols = cols;
  s->Rows = rows;
  memset(s->Back, 0, need * sizeof(Cell));
  s->ForceRepaint = true;
}

void Screen_BeginFrame(Screen* s) { memset(s->Back, 0, (u32)s->Cols * s->Rows * sizeof(Cell)); }

u32 Screen_PutChar(Screen* s, u16 x, u16 y, u32 cp, Style st) {
  if (x >= s->Cols || y >= s->Rows) return 1;
  u32 w = CharWidth(cp);
  if (w < 1) w = 1;
  u32 idx = (u32)y * s->Cols + x;
  s->Back[idx].Ch = cp;
  s->Back[idx].St = st;
  if (w == 2 && (u16)(x + 1) < s->Cols) {
    s->Back[idx + 1].Ch = CELL_CONT;
    s->Back[idx + 1].St = st;
  }
  return w;
}

u16 Screen_PutText(Screen* s, u16 x, u16 y, StrView utf8, Style st, u16 maxXExclusive) {
  if (maxXExclusive > s->Cols) maxXExclusive = s->Cols;
  u32 i = 0;
  while (i < utf8.Length && x < maxXExclusive) {
    u32 cp = Utf8Next(utf8, &i);
    u32 w = CharWidth(cp);
    if (w < 1) w = 1;
    if ((u32)x + w > maxXExclusive) break;
    Screen_PutChar(s, x, y, cp, st);
    x = (u16)(x + w);
  }
  return x;
}

void Screen_Fill(Screen* s, u16 x0, u16 y0, u16 x1, u16 y1, Style st) {
  if (x1 >= s->Cols) x1 = s->Cols ? (u16)(s->Cols - 1) : 0;
  if (y1 >= s->Rows) y1 = s->Rows ? (u16)(s->Rows - 1) : 0;
  for (u16 y = y0; y <= y1 && y < s->Rows; y++) {
    for (u16 x = x0; x <= x1 && x < s->Cols; x++) {
      u32 idx = (u32)y * s->Cols + x;
      s->Back[idx].Ch = 0;
      s->Back[idx].St = st;
    }
  }
}

void Screen_SetCursor(Screen* s, bool visible, u16 x, u16 y) {
  s->CursorVisible = visible;
  s->CursorX = x;
  s->CursorY = y;
}

void Screen_Flush(Screen* s) {
  if (!s->OutInit) {
    s->Out = StrBuf_Make(s->Mem, (u32)s->Cols * s->Rows * 4 + 256);
    s->OutInit = true;
  }
  StrBuf* b = &s->Out;
  StrBuf_Clear(b);

  if (s->ForceRepaint) {
    StrBuf_AppendCStr(b, "\x1b[2J");
    memset(s->Front, 0, (u32)s->Cols * s->Rows * sizeof(Cell));
    s->ForceRepaint = false;
  }

  bool penValid = false;
  Style pen = StyleDefault();

  for (u16 y = 0; y < s->Rows; y++) {
    Cell* fb = s->Front + (u32)y * s->Cols;
    Cell* bb = s->Back + (u32)y * s->Cols;

    i32 minx = -1, maxx = -1;
    for (u16 x = 0; x < s->Cols; x++) {
      if (!CellEq(fb[x], bb[x])) {
        if (minx < 0) minx = x;
        maxx = x;
      }
    }
    if (minx < 0) continue;  // row unchanged

    // Expand the span to whole glyphs: start on an owner, end past any tail.
    if (bb[minx].Ch == CELL_CONT && minx > 0) minx--;
    while ((u16)(maxx + 1) < s->Cols && bb[maxx + 1].Ch == CELL_CONT) maxx++;

    EmitCup(b, (u16)minx, y);
    for (i32 x = minx; x <= maxx; x++) {
      Cell c = bb[x];
      if (c.Ch == CELL_CONT) continue;  // covered by its owner glyph
      if (!penValid || !StyleEq(pen, c.St)) {
        EmitSgr(b, c.St);
        pen = c.St;
        penValid = true;
      }
      EmitUtf8(b, c.Ch);
    }
    for (i32 x = minx; x <= maxx; x++) fb[x] = bb[x];
  }

  if (penValid) StrBuf_AppendCStr(b, "\x1b[0m");

  if (s->CursorVisible) {
    EmitCup(b, s->CursorX, s->CursorY);
    StrBuf_AppendCStr(b, "\x1b[?25h");
  } else {
    StrBuf_AppendCStr(b, "\x1b[?25l");
  }

  if (b->Length && s->Write) s->Write(s->WriteUser, b->Data, b->Length);
}

Cell Screen_BackAt(const Screen* s, u16 x, u16 y) {
  if (x >= s->Cols || y >= s->Rows) return Cell{0, StyleDefault()};
  return s->Back[(u32)y * s->Cols + x];
}

Cell Screen_FrontAt(const Screen* s, u16 x, u16 y) {
  if (x >= s->Cols || y >= s->Rows) return Cell{0, StyleDefault()};
  return s->Front[(u32)y * s->Cols + x];
}
