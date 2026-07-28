#pragma once
#include "base/array.h"
#include "base/str.h"

// Reusable multi-line UTF-8 input editor. Holds a buffer (which may contain '\n'),
// a cursor as a byte offset, and a submit history. Pure / terminal-free.
struct LineEdit {
  Arena* Mem;
  StrBuf Text;
  u32 Cursor;       // byte offset in [0, Text.Length]
  u32 Anchor;       // selection anchor, or UINT32_MAX when inactive
  u16 DesiredCol;   // sticky display column for Up/Down
  Array<StrView> History;
  i32 HistPos;      // -1 = editing live buffer; else index into History
  StrBuf Stash;     // live buffer saved while browsing history
};

struct LineEditLayout {
  StrView Rows[256];
  u32 Starts[256];
  u32 Count;
  u32 CursorRow;
  u16 CursorCol;
};

void LineEdit_Init(LineEdit* e, Arena* mem);
void LineEdit_Clear(LineEdit* e);

StrView LineEdit_Text(const LineEdit* e);
u32 LineEdit_Cursor(const LineEdit* e);
bool LineEdit_Empty(const LineEdit* e);
bool LineEdit_HasSelection(const LineEdit* e);
bool LineEdit_SelectionBounds(const LineEdit* e, u32* start, u32* end);
StrView LineEdit_SelectedText(const LineEdit* e);
void LineEdit_ClearSelection(LineEdit* e);

void LineEdit_SetCursor(LineEdit* e, u32 offset, bool extend = false);
void LineEdit_Layout(const LineEdit* e, u16 wrapWidth,
                     LineEditLayout* out);
u32 LineEdit_HitTest(const LineEdit* e, u16 wrapWidth, u32 row, u32 col);

void LineEdit_InsertText(LineEdit* e, StrView s);  // paste-safe (keeps newlines)
void LineEdit_InsertChar(LineEdit* e, u32 cp);
void LineEdit_Newline(LineEdit* e);
void LineEdit_Backspace(LineEdit* e);
void LineEdit_Delete(LineEdit* e);

void LineEdit_Left(LineEdit* e, bool extend = false);
void LineEdit_Right(LineEdit* e, bool extend = false);
bool LineEdit_Up(LineEdit* e, bool extend = false);    // false at the first logical line
bool LineEdit_Down(LineEdit* e, bool extend = false);  // false at the last logical line
void LineEdit_Home(LineEdit* e, bool extend = false);
void LineEdit_End(LineEdit* e, bool extend = false);
void LineEdit_WordLeft(LineEdit* e, bool extend = false);
void LineEdit_WordRight(LineEdit* e, bool extend = false);
void LineEdit_DeleteWordBack(LineEdit* e);  // Ctrl+W
void LineEdit_KillToEnd(LineEdit* e);       // Ctrl+K
void LineEdit_KillToStart(LineEdit* e);     // Ctrl+U

bool LineEdit_OnFirstLine(const LineEdit* e);
bool LineEdit_OnLastLine(const LineEdit* e);

// History recall (driven by the controller when Up/Down hit buffer edges).
void LineEdit_HistoryPrev(LineEdit* e);
void LineEdit_HistoryNext(LineEdit* e);

// Submit: returns the current text (arena-stable), records it in history, and
// clears the buffer. Empty submissions are not recorded.
StrView LineEdit_Submit(LineEdit* e);
