#pragma once
#include "base/array.h"
#include "base/arena.h"
#include "base/str.h"

// Shared plumbing for the markdown loaders in this directory (skills, prompt
// templates). Both walk a set of directories, deduplicate by canonical path,
// and report per-file problems without failing the load.

// A file the loader skipped, or accepted with a warning.
struct LoadDiagnostic {
  StrView Path;
  StrView Message;
};

inline void LoadDiagnostic_Add(Arena* arena, Array<LoadDiagnostic>* diagnostics,
                               StrView path, StrView message) {
  if (!diagnostics) return;
  LoadDiagnostic diagnostic = {};
  diagnostic.Path = StrView_Dup(arena, path);
  diagnostic.Message = StrView_Dup(arena, message);
  Array_Push(diagnostics, diagnostic);
}
