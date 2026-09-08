#pragma once
#include "base/array.h"
#include "base/arena.h"
#include "base/config_file.h"
#include "base/str.h"
#include "tools/fs.h"

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

// The caller restores scratch after parsing the returned content.
inline bool Loader_ReadFile(Arena* arena, Arena* scratch,
                            Array<StrView>* seenPaths,
                            Array<LoadDiagnostic>* diagnostics,
                            StrView canonical, StrView tooLargeMessage,
                            StrView readErrorMessage, StrView* content) {
  for (u32 i = 0; i < seenPaths->Count; i++)
    if (Fs_PathEquals(seenPaths->Data[i], canonical)) return false;
  // Failed files are remembered too, so repeated roots do not repeat warnings.
  Array_Push(seenPaths, StrView_Dup(arena, canonical));

  FsStat stat = Fs_Stat(canonical);
  if (!stat.Exists || stat.IsDir) return false;
  if (stat.Size > ((u64)1 << 20)) {
    LoadDiagnostic_Add(arena, diagnostics, canonical, tooLargeMessage);
    return false;
  }
  if (ConfigFile_ReadAll(scratch, canonical, content) !=
      ConfigFileReadResult::Success) {
    LoadDiagnostic_Add(arena, diagnostics, canonical, readErrorMessage);
    return false;
  }
  return true;
}
