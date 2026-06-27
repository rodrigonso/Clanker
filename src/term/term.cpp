#include "term/term.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Shared (platform-independent) terminal output + exit-restore tracking.

static Term* g_active = nullptr;
static bool g_atexitArmed = false;

void Term_Write(const char* data, u32 len) {
  fwrite(data, 1, len, stdout);
  fflush(stdout);
}

void Term_WriteStr(const char* s) { Term_Write(s, (u32)strlen(s)); }

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
