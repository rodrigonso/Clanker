#include "term/term.h"

#include <string.h>
#include <windows.h>

bool Term_IsInteractive() {
  DWORD m;
  HANDLE ho = GetStdHandle(STD_OUTPUT_HANDLE);
  HANDLE hi = GetStdHandle(STD_INPUT_HANDLE);
  return GetConsoleMode(ho, &m) && GetConsoleMode(hi, &m);
}

bool Term_Enter(Term* t) {
  memset(t, 0, sizeof(*t));
  if (!Term_IsInteractive()) return false;

  HANDLE ho = GetStdHandle(STD_OUTPUT_HANDLE);
  HANDLE hi = GetStdHandle(STD_INPUT_HANDLE);
  DWORD outMode = 0, inMode = 0;
  GetConsoleMode(ho, &outMode);
  GetConsoleMode(hi, &inMode);

  t->HOut = ho;
  t->HIn = hi;
  t->SavedOutMode = outMode;
  t->SavedInMode = inMode;
  t->SavedCP = GetConsoleOutputCP();

  SetConsoleOutputCP(CP_UTF8);
  SetConsoleMode(ho, outMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);
  // Raw VT input: deliver the ANSI/VT byte stream (arrows, bracketed paste, etc.)
  // with no line buffering, echo, or Ctrl+C signal processing.
  SetConsoleMode(hi, ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_EXTENDED_FLAGS);

  t->Active = true;
  Term_TrackForRestore(t);
  Term_WriteStr(Term_SetupSequence());
  return true;
}

void Term_Exit(Term* t) {
  if (!t->Active) return;
  Term_WriteStr(Term_TeardownSequence());
  FlushConsoleInputBuffer((HANDLE)t->HIn);
  SetConsoleMode((HANDLE)t->HOut, t->SavedOutMode);
  SetConsoleMode((HANDLE)t->HIn, t->SavedInMode);
  SetConsoleOutputCP(t->SavedCP);
  t->Active = false;
}

void Term_Size(const Term* t, u16* cols, u16* rows) {
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  if (GetConsoleScreenBufferInfo((HANDLE)t->HOut, &csbi)) {
    *cols = (u16)(csbi.srWindow.Right - csbi.srWindow.Left + 1);
    *rows = (u16)(csbi.srWindow.Bottom - csbi.srWindow.Top + 1);
  } else {
    *cols = 80;
    *rows = 24;
  }
}

u32 Term_ReadInput(const Term* t, char* buf, u32 cap) {
  DWORD got = 0;
  if (!ReadFile((HANDLE)t->HIn, buf, cap, &got, nullptr)) return 0;
  return (u32)got;
}

bool Term_WaitInput(const Term* t, u32 timeoutMs) {
  return WaitForSingleObject((HANDLE)t->HIn, timeoutMs) == WAIT_OBJECT_0;
}

void Term_WakeInput(const Term* t) {
  if (!t->Active || !t->HIn) return;
  INPUT_RECORD record = {};
  record.EventType = KEY_EVENT;
  record.Event.KeyEvent.bKeyDown = TRUE;
  record.Event.KeyEvent.wRepeatCount = 1;
  record.Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
  record.Event.KeyEvent.uChar.UnicodeChar = L'\r';
  DWORD written = 0;
  WriteConsoleInputW((HANDLE)t->HIn, &record, 1, &written);
}
