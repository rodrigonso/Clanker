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

// Alt screen, clear, home, bracketed paste, SGR all-motion mouse tracking, and
// enhanced keyboard reporting. The keyboard requests are what make Shift+Enter
// distinguishable from Enter: the legacy stream sends a bare CR for both. We
// ask for xterm modifyOtherKeys level 2 and push kitty keyboard protocol flag
// 1; terminals implementing neither ignore them and keep sending legacy bytes.
static const char kSetup[] =
    "\x1b[?1049h\x1b[2J\x1b[H\x1b[?2004h\x1b[?1003h\x1b[?1006h"
    "\x1b[>4;2m\x1b[>1u";

// Reverse of kSetup; every mode set above is reset here exactly once.
static const char kTeardown[] =
    "\x1b[<1u\x1b[>4;0m"
    "\x1b[?1006l\x1b[?1003l\x1b[?2004l\x1b[?25h\x1b[?1049l";

const char* Term_SetupSequence() { return kSetup; }
const char* Term_TeardownSequence() { return kTeardown; }

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
