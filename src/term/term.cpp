#include "term/term.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crypto/base64.h"

// Shared (platform-independent) terminal output + exit-restore tracking.

static Term* g_active = nullptr;
static bool g_atexitArmed = false;

void Term_Write(const char* data, u32 len) {
  fwrite(data, 1, len, stdout);
  fflush(stdout);
}

void Term_WriteStr(const char* s) { Term_Write(s, (u32)strlen(s)); }

void Term_AppendOSC52(StrBuf* out, StrView data) {
  u32 encodedLen = ((data.Length + 2) / 3) * 4;
  StrBuf_Ensure(out, 8 + encodedLen);
  usize mark = out->Backing->Used;
  StrView encoded = Base64Encode(out->Backing, (const byte*)data.Data, data.Length);
  StrBuf_Append(out, SVL("\x1b]52;c;"));
  StrBuf_Append(out, encoded);
  StrBuf_AppendChar(out, '\x07');
  ArenaRestore(out->Backing, mark);
}

static void RestoreAtExit() {
  if (g_active && g_active->Active) Term_Exit(g_active);
}

void Term_TrackForRestore(Term* t) {
  g_active = t;
  if (!g_atexitArmed) {
    atexit(RestoreAtExit);
    g_atexitArmed = true;
  }
}
