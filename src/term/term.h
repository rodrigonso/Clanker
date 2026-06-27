#pragma once
#include "base/basics.h"

// Terminal control: raw mode + alternate screen, size query, and raw output.
// Internal fields are platform-specific but the header pulls in no OS headers.
struct Term {
  bool Active;
#if defined(_WIN32)
  void* HOut;
  void* HIn;
  u32 SavedOutMode;
  u32 SavedInMode;
  u32 SavedCP;
#else
  int InFd;
  int OutFd;
  alignas(8) unsigned char SavedTermios[64];
#endif
};

// True when both stdin and stdout are a real console/tty.
bool Term_IsInteractive();

// Enter raw mode + alternate screen + bracketed paste. Returns false (and does
// nothing) if not interactive. Restoration is also armed via atexit.
bool Term_Enter(Term* t);

// Restore the terminal to its prior state. Idempotent.
void Term_Exit(Term* t);

// Current viewport size in character cells.
void Term_Size(const Term* t, u16* cols, u16* rows);

// Raw byte output to stdout (no newline translation), flushed immediately.
void Term_Write(const char* data, u32 len);
void Term_WriteStr(const char* s);

// Blocking read of up to `cap` raw input bytes (VT/ANSI stream). Returns the
// number of bytes read, or 0 on EOF/error.
u32 Term_ReadInput(const Term* t, char* buf, u32 cap);

// Internal: arrange Term_Exit to run on normal process exit.
void Term_TrackForRestore(Term* t);

#if defined(_WIN32)
void* Term_InHandle(const Term* t);
#else
int Term_InFd(const Term* t);
#endif
