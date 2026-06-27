#include "base/arena.h"
#include "check.h"
#include "term/lineedit.h"

static bool Eq(const LineEdit* e, const char* s) { return StrView_EqCStr(LineEdit_Text(e), s); }

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

  // History recall.
  LineEdit_Clear(&e);
  LineEdit_InsertText(&e, SVL("first"));
  StrView s1 = LineEdit_Submit(&e);
  CHECK(StrView_EqCStr(s1, "first"));
  CHECK(LineEdit_Empty(&e));
  LineEdit_InsertText(&e, SVL("second"));
  LineEdit_Submit(&e);
  LineEdit_HistoryPrev(&e);
  CHECK(Eq(&e, "second"));
  LineEdit_HistoryPrev(&e);
  CHECK(Eq(&e, "first"));
  LineEdit_HistoryNext(&e);
  CHECK(Eq(&e, "second"));
  LineEdit_HistoryNext(&e);
  CHECK(Eq(&e, ""));  // back to the (empty) live buffer

  ArenaRelease(&a);
}
