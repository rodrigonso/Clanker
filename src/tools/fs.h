#pragma once
#include "base/arena.h"
#include "base/array.h"
#include "base/str.h"

struct FsStat {
  bool Exists;
  bool IsDir;
  u64 Size;
};

FsStat Fs_Stat(StrView path);
bool Fs_Exists(StrView path);
bool Fs_IsDir(StrView path);
bool Fs_MkdirP(Arena* arena, StrView path);
StrView Fs_RealPath(Arena* arena, StrView path);

// Path utilities are pure string operations. Fs_Ext returns a raw slice of the
// original path and does not lowercase.
//
// Fs_PathFold normalizes one character for path comparison: separators become
// '/', and on Windows upper case folds to lower. Fs_PathEquals compares two
// paths under that folding.
bool Fs_IsSep(char c);
char Fs_PathFold(char c);
bool Fs_PathEquals(StrView a, StrView b);
StrView Fs_Parent(StrView path);
StrView Fs_Base(StrView path);
StrView Fs_Ext(StrView path);
StrView Fs_Join(Arena* arena, StrView a, StrView b);
StrView Fs_ToPosix(Arena* arena, StrView path);
StrView Fs_Relativize(Arena* arena, StrView root, StrView path);

struct FsEntry {
  StrView Name;
  bool IsDir;
  u64 Size;
};

Array<FsEntry> Fs_ListDir(Arena* arena, StrView dir, bool* ok);

struct FsWalkOptions {
  bool RespectGitignore;
  bool IncludeDirs;
};

typedef bool (*FsWalkVisitor)(void* user, StrView absPath, StrView relPath, const FsEntry& e);
void Fs_Walk(Arena* arena, StrView root, const FsWalkOptions& opts, FsWalkVisitor visit, void* user);
