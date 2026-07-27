#include "base/arena.h"
#include "check.h"
#include "term/lineedit.h"

static bool Eq(const LineEdit* e, const char* s) { return StrView_EqCStr(LineEdit_Text(e), s); }

static void Reset(LineEdit* e, StrView text) {
  LineEdit_Clear(e);
  LineEdit_InsertText(e, text);
}

static void Select(LineEdit* e, u32 anchor, u32 cursor) {
  LineEdit_SetCursor(e, anchor);
  LineEdit_SetCursor(e, cursor, true);
}

static bool SelectionEq(const LineEdit* e, u32 start, u32 end, const char* text) {
  u32 actualStart, actualEnd;
  return LineEdit_SelectionBounds(e, &actualStart, &actualEnd) && actualStart == start &&
         actualEnd == end && StrView_EqCStr(LineEdit_SelectedText(e), text);
}

void TestLineEdit() {
  Arena a = ArenaCreate();
  LineEdit e;
  LineEdit_Init(&e, &a);

  // Insertion + horizontal motion.
  LineEdit_InsertText(&e, SVL("hello world"));
  CHECK(Eq(&e, "hello world"));
  CHECK(LineEdit_Cursor(&e) == 11);
  LineEdit_Home(&e);
  CHECK(LineEdit_Cursor(&e) == 0);
  LineEdit_End(&e);
  CHECK(LineEdit_Cursor(&e) == 11);
  LineEdit_Left(&e);
  LineEdit_Left(&e);
  CHECK(LineEdit_Cursor(&e) == 9);
  LineEdit_InsertChar(&e, 'X');
  CHECK(Eq(&e, "hello worXld"));
  LineEdit_Backspace(&e);
  CHECK(Eq(&e, "hello world"));

  // Word navigation.
  LineEdit_End(&e);
  LineEdit_WordLeft(&e);
  CHECK(LineEdit_Cursor(&e) == 6);  // start of "world"
  LineEdit_WordLeft(&e);
  CHECK(LineEdit_Cursor(&e) == 0);

  // Multi-line: vertical motion keeps the desired column.
  LineEdit_Clear(&e);
  LineEdit_InsertText(&e, SVL("ab\ncd"));
  CHECK(!LineEdit_OnFirstLine(&e));
  CHECK(LineEdit_OnLastLine(&e));
  CHECK(LineEdit_Up(&e));
  CHECK(LineEdit_Cursor(&e) == 2);  // end of "ab"
  CHECK(!LineEdit_Up(&e));          // already on first line
  CHECK(LineEdit_Down(&e));
  CHECK(LineEdit_Cursor(&e) == 5);  // end of "cd"

  // Backspace merges lines.
  LineEdit_Backspace(&e);
  LineEdit_Backspace(&e);
  LineEdit_Backspace(&e);
  CHECK(Eq(&e, "ab"));

  // Newline insertion.
  LineEdit_Clear(&e);
  LineEdit_InsertText(&e, SVL("x"));
  LineEdit_Newline(&e);
  LineEdit_InsertText(&e, SVL("y"));
  CHECK(Eq(&e, "x\ny"));

  // Kill operations.
  LineEdit_Clear(&e);
  LineEdit_InsertText(&e, SVL("foo bar"));
  LineEdit_DeleteWordBack(&e);
  CHECK(Eq(&e, "foo "));
  LineEdit_Clear(&e);
  LineEdit_InsertText(&e, SVL("hello"));
  LineEdit_Home(&e);
  LineEdit_KillToEnd(&e);
  CHECK(Eq(&e, ""));

  // UTF-8 aware motion ('é' is two bytes).
  LineEdit_Clear(&e);
  LineEdit_InsertText(&e, SVL("\xc3\xa9"));
  CHECK(LineEdit_Cursor(&e) == 2);
  LineEdit_Left(&e);
  CHECK(LineEdit_Cursor(&e) == 0);
  LineEdit_Right(&e);
  CHECK(LineEdit_Cursor(&e) == 2);

  // Selection bounds and directional collapse.
  Reset(&e, SVL("hello"));
  LineEdit_SetCursor(&e, 0);
  LineEdit_Left(&e, true);
  LineEdit_Right(&e, true);
  CHECK(SelectionEq(&e, 0, 1, "h"));
  LineEdit_SetCursor(&e, 1);
  LineEdit_Right(&e, true);
  LineEdit_Right(&e, true);
  LineEdit_Right(&e, true);
  CHECK(SelectionEq(&e, 1, 4, "ell"));
  LineEdit_Left(&e);
  CHECK(LineEdit_Cursor(&e) == 1);
  CHECK(!LineEdit_HasSelection(&e));

  Select(&e, 4, 1);
  CHECK(LineEdit_Cursor(&e) == 1);
  CHECK(SelectionEq(&e, 1, 4, "ell"));
  LineEdit_Right(&e);
  CHECK(LineEdit_Cursor(&e) == 4);
  CHECK(!LineEdit_HasSelection(&e));

  Reset(&e, SVL("one two\nthree"));
  LineEdit_SetCursor(&e, 4);
  LineEdit_WordRight(&e, true);
  CHECK(StrView_EqCStr(LineEdit_SelectedText(&e), "two"));
  LineEdit_WordLeft(&e);
  CHECK(LineEdit_Cursor(&e) == 4);
  CHECK(!LineEdit_HasSelection(&e));
  LineEdit_Home(&e, true);
  CHECK(StrView_EqCStr(LineEdit_SelectedText(&e), "one "));
  LineEdit_SetCursor(&e, 4);
  LineEdit_End(&e, true);
  CHECK(StrView_EqCStr(LineEdit_SelectedText(&e), "two"));

  // Cursor placement and hit-testing preserve UTF-8 boundaries.
  Reset(&e, SVL("a\xc3\xa9\xe4\xb8\xadz"));
  LineEdit_SetCursor(&e, 2);  // middle of 'é'
  CHECK(LineEdit_Cursor(&e) == 1);
  LineEdit_Right(&e, true);
  CHECK(LineEdit_Cursor(&e) == 3);
  CHECK(StrView_EqCStr(LineEdit_SelectedText(&e), "\xc3\xa9"));

  Reset(&e, SVL("a\xe4\xb8\xad" "bcd"));
  CHECK(LineEdit_HitTest(&e, 4, 0, 1) == 1);
  CHECK(LineEdit_HitTest(&e, 4, 0, 2) == 4);  // second cell of wide glyph
  CHECK(LineEdit_HitTest(&e, 4, 1, 0) == 5);
  CHECK(LineEdit_HitTest(&e, 4, 1, 2) == 7);
  LineEdit_SetCursor(&e, LineEdit_HitTest(&e, 4, 0, 1));
  LineEdit_SetCursor(&e, LineEdit_HitTest(&e, 4, 1, 1), true);
  CHECK(SelectionEq(&e, 1, 6, "\xe4\xb8\xad" "bc"));

  Reset(&e, SVL("ab\ncd"));
  CHECK(LineEdit_HitTest(&e, 20, 0, 20) == 2);
  CHECK(LineEdit_HitTest(&e, 20, 1, 1) == 4);
  CHECK(LineEdit_HitTest(&e, 20, 9, 0) == 5);

  // Editing operations replace selections.
  Reset(&e, SVL("abcdef"));
  Select(&e, 2, 4);
  LineEdit_InsertText(&e, SVL("XY"));
  CHECK(Eq(&e, "abXYef"));
  CHECK(LineEdit_Cursor(&e) == 4);
  CHECK(!LineEdit_HasSelection(&e));
  Select(&e, 2, 4);
  LineEdit_Newline(&e);
  CHECK(Eq(&e, "ab\nef"));
  Select(&e, 2, 3);
  LineEdit_InsertText(&e, SVL("one\ntwo"));
  CHECK(Eq(&e, "abone\ntwoef"));

  Reset(&e, SVL("abcdef"));
  struct SelectionEditCase {
    void (*Apply)(LineEdit*);
    u32 Start;
    u32 End;
    const char* Expected;
  };
  SelectionEditCase edits[] = {
      {LineEdit_Backspace, 2, 4, "abef"}, {LineEdit_Delete, 1, 2, "aef"},
      {LineEdit_DeleteWordBack, 1, 2, "af"}, {LineEdit_KillToEnd, 0, 1, "f"},
      {LineEdit_KillToStart, 0, 1, ""},
  };
  for (u32 i = 0; i < 5; i++) {
    Select(&e, edits[i].Start, edits[i].End);
    edits[i].Apply(&e);
    CHECK(Eq(&e, edits[i].Expected));
  }

  Reset(&e, SVL("abcd\nxy\nwxyz"));
  LineEdit_SetCursor(&e, 3);
  CHECK(LineEdit_Down(&e, true));
  CHECK(LineEdit_Cursor(&e) == 7);
  CHECK(LineEdit_Down(&e, true));
  CHECK(LineEdit_Cursor(&e) == 11);
  CHECK(SelectionEq(&e, 3, 11, "d\nxy\nwxy"));
  CHECK(LineEdit_Up(&e));
  CHECK(LineEdit_Cursor(&e) == 3);  // unmodified Up collapses toward the start
  CHECK(!LineEdit_HasSelection(&e));
  LineEdit_SetCursor(&e, 11);
  CHECK(LineEdit_Up(&e, true));
  CHECK(LineEdit_Cursor(&e) == 7);
  CHECK(LineEdit_Up(&e, true));
  CHECK(LineEdit_Cursor(&e) == 3);
  CHECK(LineEdit_Down(&e));
  CHECK(LineEdit_Cursor(&e) == 11);  // unmodified Down collapses toward the end

  // Sticky columns match the composer's minimum-one-cell scalar width.
  Reset(&e, SVL("a\xcc\x81" "b\nxyz"));
  LineEdit_SetCursor(&e, 4);
  CHECK(LineEdit_Down(&e));
  CHECK(LineEdit_Cursor(&e) == 8);  // displayed column 3
  CHECK(LineEdit_Up(&e));
  CHECK(LineEdit_Cursor(&e) == 4);

  Reset(&e, SVL("a\xe4\xb8\xad\nxyz"));
  LineEdit_SetCursor(&e, 4);
  CHECK(LineEdit_Down(&e));
  CHECK(LineEdit_Cursor(&e) == 8);  // 'a' + wide glyph is also column 3

  // History recall.
  LineEdit_Clear(&e);
  LineEdit_InsertText(&e, SVL("first"));
  StrView s1 = LineEdit_Submit(&e);
  CHECK(StrView_EqCStr(s1, "first"));
  CHECK(LineEdit_Empty(&e));
  LineEdit_InsertText(&e, SVL("second"));
  LineEdit_Home(&e);
  LineEdit_End(&e, true);
  LineEdit_Submit(&e);
  CHECK(!LineEdit_HasSelection(&e));
  LineEdit_HistoryPrev(&e);
  CHECK(Eq(&e, "second"));
  LineEdit_Home(&e);
  LineEdit_Right(&e, true);
  CHECK(LineEdit_HasSelection(&e));
  LineEdit_HistoryPrev(&e);
  CHECK(Eq(&e, "first"));
  CHECK(!LineEdit_HasSelection(&e));
  LineEdit_HistoryNext(&e);
  CHECK(Eq(&e, "second"));
  LineEdit_HistoryNext(&e);
  CHECK(Eq(&e, ""));  // back to the (empty) live buffer
  CHECK(!LineEdit_HasSelection(&e));

  LineEdit_InsertText(&e, SVL("draft"));
  LineEdit_HistoryPrev(&e);
  LineEdit_HistoryNext(&e);
  CHECK(Eq(&e, "draft"));

  Reset(&e, SVL("reset"));
  Select(&e, 0, 5);
  CHECK(LineEdit_HasSelection(&e));
  LineEdit_Clear(&e);
  CHECK(LineEdit_Empty(&e));
  CHECK(!LineEdit_HasSelection(&e));

  ArenaRelease(&a);
}
