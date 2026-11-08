#pragma once
#include "base/arena.h"
#include "base/array.h"
#include "base/str.h"

struct FsStat {
  bool Exists;
  bool IsDir;
  u64 Size;
  bool IsRegular;
  // Last write time in Unix milliseconds; 0 when unknown.
  u64 ModifiedMs;
};

FsStat Fs_Stat(StrView path);
bool Fs_Exists(StrView path);
bool Fs_IsDir(StrView path);
bool Fs_MkdirP(Arena* arena, StrView path);
StrView Fs_RealPath(Arena* arena, StrView path);
// Deletes a file or an empty directory.
bool Fs_Remove(Arena* arena, StrView path);
// Deletes `path` and everything below it. A symlink or junction inside it is
// removed itself, never followed, so the tree cannot reach outside `path`.
// A missing path succeeds.
bool Fs_RemoveTree(Arena* arena, StrView path);

// The most a file tool (read, edit) buffers from one file. They hold the whole
// file plus working copies, so larger files are left to tools that stream.
constexpr u64 FS_TOOL_MAX_FILE_BYTES = (u64)64 << 20;

struct FsPermissionTarget {
  StrView Path;
  bool Contained;
  bool Protected;
};

struct ToolArgs;
struct ToolPermissionInfo;
struct ToolPermissionScope;

// Canonicalizes existing path components, retaining missing trailing
// components so callers can classify a future write without treating it as an
// existing path. A relative target resolves against the scope's working
// directory; it is contained when it lies inside that directory or one of the
// scope's writable roots. Ambiguous paths, and roots that do not exist, are
// never classified as contained.
//
// `Protected` marks a contained path with a `.git`, `.clanker` or `.agents`
// component below every root that contains it (ASCII case-insensitive). Writes
// there change what runs later -- git hooks and config, project hooks, skills
// -- so they are not auto-allowed even inside the workspace.
FsPermissionTarget Fs_ResolvePermissionTarget(Arena* arena,
                                              const ToolPermissionScope* scope,
                                              StrView target);
bool Fs_DescribeMutationPermission(Arena* arena, const ToolArgs* args,
                                   const ToolPermissionScope* scope,
                                   StrView actionPrefix,
                                   StrView grantPrefix,
                                   ToolPermissionInfo* out, StrBuf* err);

// `path` canonicalized exactly as permission checks see a tool target, so a
// containment test between the two agrees with what the engine allowed.
StrView Fs_PermissionPath(Arena* arena, StrView workingDirectory, StrView path);

// The canonical target of a `write` or `edit` call; empty for any other tool
// or a call without a path.
StrView Fs_MutationTarget(Arena* arena, StrView toolName, const ToolArgs* args,
                          StrView workingDirectory);

// Inclusive, separator-aware containment check for already canonicalized
// paths. On Windows this comparison is case-insensitive.
bool Fs_PathContained(StrView root, StrView path);

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
  bool IsSymlink;
  bool IsRegular;
};

Array<FsEntry> Fs_ListDir(Arena* arena, StrView dir, bool* ok);

// One parsed ignore-file rule. `BaseRel` is the ignore file's directory
// relative to the scan root, so a nested rule only applies below its own
// directory. Patterns are gitignore-flavored: a leading '/' anchors to
// `BaseRel`, as does any interior '/'; a trailing '/' restricts the rule to
// directories. Last matching rule wins; Negated re-includes an entry, but
// cannot re-open an ignored ancestor directory.
struct FsIgnorePattern {
  StrView BaseRel;
  StrView Pattern;
  bool DirOnly;
  bool Anchored;
  bool Negated;
};

// Appends the rules from `<dirAbs>/<fileName>` to `patterns`, tagged with
// `dirRel`. A missing file is not an error.
void Fs_LoadIgnoreFile(Arena* arena, Array<FsIgnorePattern>* patterns,
                       StrView dirAbs, StrView dirRel, StrView fileName);
bool Fs_IsIgnored(const Array<FsIgnorePattern>* patterns, StrView relPath, bool isDir);

// Excluded directories are not descended into. False reports an error in err.
typedef bool (*FsWalkFilter)(void* user, StrView relPath, const FsEntry& entry,
                             bool* include, StrBuf* err);

struct FsWalkOptions {
  bool RespectGitignore;
  bool IncludeDirs;
  bool SkipLinks;
  bool RespectIgnoreFiles;
  bool LoadParentIgnores;
  volatile i32* Abort;
  FsWalkFilter Filter;
  void* FilterUser;
};

// Visitor paths and entry names are borrowed until the callback returns.
// Copy retained results into the caller's arena. Directory links are never
// descended into, even when SkipLinks is false.
typedef bool (*FsWalkVisitor)(void* user, StrView absPath, StrView relPath, const FsEntry& e);
void Fs_Walk(Arena* arena, StrView root, const FsWalkOptions& opts, FsWalkVisitor visit, void* user);
// False means I/O failure, cancellation, or a traversal/glob resource limit.
// A visitor returning false requests a successful early stop.
// Parent ignores stop at the nearest .git marker or filesystem root. .ignore
// outranks .gitignore, including across scopes. Limits: 128 directory levels,
// 4095-byte paths, 64 MiB scratch budget, 1 MiB/4096 rules per ignore file.
bool Fs_WalkChecked(Arena* arena, StrView root, const FsWalkOptions& opts,
                    FsWalkVisitor visit, void* user, StrBuf* err);
