#include "base/sysinfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/file_io.h"

#if defined(_WIN32)
#include <windows.h>
// psapi.h must follow windows.h
#include <psapi.h>
#include "base/win32_utf8.h"
#else
#include <limits.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(_WIN32)
// FILETIME counts 100 ns ticks from 1601-01-01; this is the gap to the Unix
// epoch in the same units.
static const u64 FILETIME_EPOCH_DELTA = 116444736000000000ull;

u64 NowMillis() {
  FILETIME ft = {};
  GetSystemTimeAsFileTime(&ft);
  u64 ticks = ((u64)ft.dwHighDateTime << 32) | (u64)ft.dwLowDateTime;
  if (ticks < FILETIME_EPOCH_DELTA) return 0;
  return (ticks - FILETIME_EPOCH_DELTA) / 10000ull;
}

u64 PeakRssBytes() {
  PROCESS_MEMORY_COUNTERS pmc = {};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return (u64)pmc.PeakWorkingSetSize;
  return 0;
}

u64 CurrentRssBytes() {
  PROCESS_MEMORY_COUNTERS pmc = {};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
    return (u64)pmc.WorkingSetSize;
  return 0;
}
#else
u64 NowMillis() {
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return (u64)time(nullptr) * 1000ull;
  return (u64)ts.tv_sec * 1000ull + (u64)(ts.tv_nsec / 1000000L);
}

u64 PeakRssBytes() {
  struct rusage r;
  if (getrusage(RUSAGE_SELF, &r) != 0) return 0;
  return (u64)r.ru_maxrss;  // bytes on macOS
}

u64 CurrentRssBytes() {
  mach_task_basic_info_data_t info = {};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  kern_return_t result =
      task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info,
                &count);
  return result == KERN_SUCCESS ? (u64)info.resident_size : 0;
}
#endif

// ---- path helpers (no std) -----------------------------------------------
namespace {

bool IsSep(char c) { return c == '/' || c == '\\'; }

char PathSep() {
#if defined(_WIN32)
  return '\\';
#else
  return '/';
#endif
}

bool IsAbsPath(StrView p) {
  if (p.Length == 0) return false;
  if (IsSep(p.Data[0])) return true;
#if defined(_WIN32)
  if (p.Length >= 2 && p.Data[1] == ':') return true;
#endif
  return false;
}

// Copy `s` into `a` and append a null terminator so the result is usable as a
// C string (via .Data) while the returned Length excludes the terminator.
StrView DupCStr(Arena* a, StrView s) {
  return StrView{StrView_DupCStr(a, s), s.Length};
}

// Strip any trailing path separators (view slice only).
StrView StripTrailingSep(StrView p) {
  u32 e = p.Length;
  while (e > 0 && IsSep(p.Data[e - 1])) e--;
  return StrView{p.Data, e};
}

// Parent directory of `p` (one segment up), as a slice of `p`. Returns an empty
// view once no further parent exists.
StrView ParentDir(StrView p) {
  u32 e = p.Length;
  while (e > 0 && IsSep(p.Data[e - 1])) e--;      // trailing separators
  while (e > 0 && !IsSep(p.Data[e - 1])) e--;     // last segment
  while (e > 0 && IsSep(p.Data[e - 1])) e--;      // separators before it
  return StrView{p.Data, e};
}

// Join base + leaf into a fresh null-terminated buffer in `a`.
StrBuf JoinPath(Arena* a, StrView base, StrView leaf) {
  StrBuf b = StrBuf_Make(a, base.Length + leaf.Length + 2);
  StrBuf_Append(&b, base);
  if (base.Length > 0 && !IsSep(base.Data[base.Length - 1])) StrBuf_AppendChar(&b, PathSep());
  StrBuf_Append(&b, leaf);
  return b;
}

enum class PathKind { Missing, Dir, File };

PathKind StatKind(const char* path) {
#if defined(_WIN32)
  wchar_t native[4096];
  if (!Win32_Path(SV(path), native, 4096)) return PathKind::Missing;
  DWORD at = GetFileAttributesW(native);
  if (at == INVALID_FILE_ATTRIBUTES) return PathKind::Missing;
  return (at & FILE_ATTRIBUTE_DIRECTORY) ? PathKind::Dir : PathKind::File;
#else
  struct stat st;
  if (stat(path, &st) != 0) return PathKind::Missing;
  return S_ISDIR(st.st_mode) ? PathKind::Dir : PathKind::File;
#endif
}

// .git files and HEAD are a line or two; anything bigger is not one.
constexpr u64 GIT_SMALL_FILE_MAX_BYTES = 64 * 1024;

// Read an entire (small) file into `a`. Empty view on failure.
StrView ReadSmallFile(Arena* a, const char* path) {
  StrView content = {};
  if (File_ReadAll(a, SV(path), &content, GIT_SMALL_FILE_MAX_BYTES) !=
      FileIoResult::Success)
    return StrView{nullptr, 0};
  return content;
}

bool StartsWithAt(StrView s, u32 i, StrView pfx) {
  if (i + pfx.Length > s.Length) return false;
  return memcmp(s.Data + i, pfx.Data, pfx.Length) == 0;
}

bool HasHomePrefix(StrView path, StrView home) {
  if (home.Length == 0 || path.Length < home.Length) return false;
  for (u32 i = 0; i < home.Length; i++) {
    char pc = path.Data[i], hc = home.Data[i];
#if defined(_WIN32)
    if (IsSep(pc) && IsSep(hc)) continue;
    if (Ascii_ToLower(pc) != Ascii_ToLower(hc)) return false;
#else
    if (pc != hc) return false;
#endif
  }
  return path.Length == home.Length || IsSep(path.Data[home.Length]);
}

}  // namespace

StrView Sys_CurrentDir(Arena* a) {
#if defined(_WIN32)
  wchar_t buf[4096];
  DWORD length = GetCurrentDirectoryW(4096, buf);
  if (!length || length >= 4096) return StrView{nullptr, 0};
  StrView utf8 = {};
  if (!Win32_Utf8(a, buf, length, &utf8)) return StrView{nullptr, 0};
  StrView trimmed = StripTrailingSep(utf8);
  // Win32_Utf8 allocated the buffer, so terminating the trimmed view in place
  // keeps .Data usable as a C string without a second copy.
  ((char*)trimmed.Data)[trimmed.Length] = 0;
  return trimmed;
#else
  char buf[4096];
  if (!getcwd(buf, sizeof(buf))) return StrView{nullptr, 0};
  return DupCStr(a, StripTrailingSep(SV(buf)));
#endif
}

StrView Sys_ExecutablePath(Arena* a) {
  Arena scratch = ArenaCreate();
  StrView result = {};
#if defined(_WIN32)
  for (u32 capacity = 512; capacity <= 32768; capacity *= 2) {
    wchar_t* path = ArenaPushArray(&scratch, wchar_t, capacity);
    DWORD length = GetModuleFileNameW(nullptr, path, capacity);
    if (!length) break;
    if (length == capacity) continue;
    Win32_Utf8(a, path, length, &result);
    break;
  }
#else
  uint32_t capacity = 512;
  char* path = ArenaPushArray(&scratch, char, capacity);
  int status = _NSGetExecutablePath(path, &capacity);
  if (status != 0 && capacity > 0 && capacity <= 1024 * 1024) {
    path = ArenaPushArray(&scratch, char, capacity);
    status = _NSGetExecutablePath(path, &capacity);
  }
  if (status == 0) {
    char* resolved = ArenaPushArray(&scratch, char, PATH_MAX);
    if (realpath(path, resolved)) result = DupCStr(a, SV(resolved));
  }
#endif
  ArenaRelease(&scratch);
  return result;
}

bool Sys_SetCurrentDir(StrView path) {
  if (!path.Data || !path.Length || path.Length >= 4096) return false;
#if defined(_WIN32)
  wchar_t native[4096];
  return Win32_Path(path, native, 4096) && SetCurrentDirectoryW(native) != 0;
#else
  char native[4096];
  memcpy(native, path.Data, path.Length);
  native[path.Length] = 0;
  return chdir(native) == 0;
#endif
}

StrView Sys_HomeDir(Arena* a) {
#if defined(_WIN32)
  const wchar_t* h = _wgetenv(L"USERPROFILE");
  if (!h || !*h) return StrView{nullptr, 0};
  StrView utf8 = {};
  if (!Win32_Utf8(a, h, (u32)wcslen(h), &utf8)) return StrView{nullptr, 0};
  StrView trimmed = StripTrailingSep(utf8);
  ((char*)trimmed.Data)[trimmed.Length] = 0;
  return trimmed;
#else
  const char* h = getenv("HOME");
  if (!h || !*h) return StrView{nullptr, 0};
  return DupCStr(a, StripTrailingSep(SV(h)));
#endif
}

StrView Sys_AbbreviatePath(Arena* a, StrView path, StrView home) {
  if (!HasHomePrefix(path, home)) return DupCStr(a, path);
  StrBuf b = StrBuf_Make(a, path.Length - home.Length + 2);
  StrBuf_AppendChar(&b, '~');
  StrBuf_Append(&b, StrView{path.Data + home.Length, path.Length - home.Length});
  return StrBuf_View(&b);
}

StrView Git_GitdirFromFile(StrView c) {
  StrView key = SVL("gitdir:");
  u32 pos = 0;
  bool found = false;
  for (u32 i = 0; i + key.Length <= c.Length; i++) {
    if (memcmp(c.Data + i, key.Data, key.Length) == 0) {
      pos = i;
      found = true;
      break;
    }
  }
  if (!found) return StrView{nullptr, 0};
  u32 i = pos + key.Length;
  while (i < c.Length && (c.Data[i] == ' ' || c.Data[i] == '\t')) i++;
  u32 start = i;
  while (i < c.Length && c.Data[i] != '\n' && c.Data[i] != '\r') i++;
  u32 end = i;
  while (end > start && (c.Data[end - 1] == ' ' || c.Data[end - 1] == '\t')) end--;
  while (end > start && IsSep(c.Data[end - 1])) end--;
  return StrView{c.Data + start, end - start};
}

StrView Git_BranchFromHead(StrView c) {
  u32 i = 0;
  while (i < c.Length && c.Data[i] == ' ') i++;
  if (!StartsWithAt(c, i, SVL("ref:"))) return StrView{nullptr, 0};  // detached HEAD
  i += 4;
  while (i < c.Length && c.Data[i] == ' ') i++;
  StrView heads = SVL("refs/heads/");
  if (!StartsWithAt(c, i, heads)) return StrView{nullptr, 0};
  i += heads.Length;
  u32 start = i;
  u32 end = c.Length;
  while (end > start && (c.Data[end - 1] == '\n' || c.Data[end - 1] == '\r' ||
                         c.Data[end - 1] == ' ' || c.Data[end - 1] == '\t'))
    end--;
  return StrView{c.Data + start, end - start};
}

StrView Git_CommonDirFromFile(StrView c) {
  u32 end = 0;
  while (end < c.Length && c.Data[end] != '\n' && c.Data[end] != '\r') end++;
  u32 start = 0;
  while (start < end && (c.Data[start] == ' ' || c.Data[start] == '\t')) start++;
  while (end > start && (c.Data[end - 1] == ' ' || c.Data[end - 1] == '\t')) end--;
  while (end > start && IsSep(c.Data[end - 1])) end--;
  return StrView{c.Data + start, end - start};
}

namespace {

// The git directory of the work tree containing `dir`: `.git` itself, or the
// target of a `.git` file (linked worktree, submodule). The walk stops at the
// first `.git` found, so an unreadable `.git` file yields empty rather than
// an outer repository. `*workTree` receives the directory holding that `.git`.
StrView FindGitDir(Arena* a, StrView dir, StrView* workTree = nullptr) {
  StrView cur = StripTrailingSep(dir);
  for (u32 guard = 0; guard < 260 && cur.Length > 0; guard++) {
    StrBuf dot = JoinPath(a, cur, SVL(".git"));
    PathKind kind = StatKind(StrBuf_CStr(&dot));
    if (kind != PathKind::Missing && workTree) *workTree = DupCStr(a, cur);
    if (kind == PathKind::Dir) return StrBuf_View(&dot);
    if (kind != PathKind::Missing) {
      StrView gp = Git_GitdirFromFile(ReadSmallFile(a, StrBuf_CStr(&dot)));
      if (gp.Length == 0) return StrView{nullptr, 0};
      if (IsAbsPath(gp)) return DupCStr(a, gp);
      StrBuf joined = JoinPath(a, cur, gp);
      return StrBuf_View(&joined);
    }
    StrView parent = ParentDir(cur);
    if (parent.Length >= cur.Length) break;  // no progress -> at the root
    cur = parent;
  }
  return StrView{nullptr, 0};
}

}  // namespace

StrView Sys_GitBranch(Arena* a, StrView dir) {
  StrView gitdir = FindGitDir(a, dir);
  if (!gitdir.Length) return StrView{nullptr, 0};
  StrBuf head = JoinPath(a, gitdir, SVL("HEAD"));
  StrView br = Git_BranchFromHead(ReadSmallFile(a, StrBuf_CStr(&head)));
  return br.Length ? DupCStr(a, br) : StrView{nullptr, 0};
}

StrView Sys_GitCommonDir(Arena* a, StrView dir, StrView* workTree) {
  if (workTree) *workTree = StrView{nullptr, 0};
  StrView found = StrView{nullptr, 0};
  StrView gitdir = FindGitDir(a, dir, &found);
  if (!gitdir.Length) return StrView{nullptr, 0};
  if (workTree) *workTree = found;
  StrBuf commondir = JoinPath(a, gitdir, SVL("commondir"));
  StrView common = Git_CommonDirFromFile(ReadSmallFile(a, StrBuf_CStr(&commondir)));
  if (!common.Length) return gitdir;
  if (IsAbsPath(common)) return DupCStr(a, common);
  StrBuf joined = JoinPath(a, gitdir, common);
  return StrBuf_View(&joined);
}

bool Sys_LocalTime(u64 timestampMs, struct tm* out) {
  if (!out) return false;
  time_t seconds = (time_t)(timestampMs / 1000ull);
  *out = tm{};
#if defined(_WIN32)
  return localtime_s(out, &seconds) == 0;
#else
  return localtime_r(&seconds, out) != nullptr;
#endif
}