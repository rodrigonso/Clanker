#pragma once
#include "base/arena.h"
#include "base/str.h"
#include "term/cell.h"

// Double-buffered character grid with a diffing painter. Owned by a single
// (render) thread. Each frame: BeginFrame -> compose into the back buffer ->
// Flush diffs back vs front and emits minimal VT to the write sink.

typedef void (*ScreenWriter)(void* user, const char* data, u32 len);

struct Screen {
  Arena* Mem;
  u16 Cols, Rows;
  Cell* Front;
  Cell* Back;
  u32 Cap;  // allocated cell capacity (high-water across resizes)
  ScreenWriter Write;
  void* WriteUser;
  bool ForceRepaint;
  bool CursorVisible;
  u16 CursorX, CursorY;
  StrBuf Out;  // reused per-flush output scratch
  bool OutInit;
};

void Screen_Init(Screen* s, Arena* mem, ScreenWriter w, void* user);
void Screen_Resize(Screen* s, u16 cols, u16 rows);
void Screen_BeginFrame(Screen* s);  // clear the back buffer to blanks

// Place a glyph; returns its column advance (1 or 2). Out-of-range is ignored.
u32 Screen_PutChar(Screen* s, u16 x, u16 y, u32 cp, Style st);

// Draw UTF-8 text on one row from x, clipped to [x, maxXExclusive). Returns the
// next free column.
u16 Screen_PutText(Screen* s, u16 x, u16 y, StrView utf8, Style st, u16 maxXExclusive);

// Fill an inclusive rectangle with blank cells of style `st` (e.g. a bar bg).
void Screen_Fill(Screen* s, u16 x0, u16 y0, u16 x1, u16 y1, Style st);

void Screen_SetCursor(Screen* s, bool visible, u16 x, u16 y);
void Screen_Flush(Screen* s);  // diff -> minimal VT -> update front

// Inspection (tests).
Cell Screen_BackAt(const Screen* s, u16 x, u16 y);
Cell Screen_FrontAt(const Screen* s, u16 x, u16 y);
