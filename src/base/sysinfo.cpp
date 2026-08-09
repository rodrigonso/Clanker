#include "base/sysinfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
// psapi.h must follow windows.h
#include <psapi.h>
#include <direct.h>
#else
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
#include <sys/resource.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

u64 NowMillis() {
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return (u64)time(nullptr) * 1000ull;
  return (u64)ts.tv_sec * 1000ull + (u64)(ts.tv_nsec / 1000000L);
}

u64 PeakRssBytes() {
  struct rusage r;
  if (getrusage(RUSAGE_SELF, &r) != 0) return 0;
#if defined(__APPLE__)
  return (u64)r.ru_maxrss;  // bytes on macOS
#else
  return (u64)r.ru_maxrss * 1024;  // kilobytes on Linux
#endif
}

u64 CurrentRssBytes() {
#if defined(__APPLE__)
  mach_task_basic_info_data_t info = {};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  kern_return_t result =
      task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info,
                &count);
  return result == KERN_SUCCESS ? (u64)info.resident_size : 0;
#elif defined(__linux__)
  FILE* file = fopen("/proc/self/statm", "rb");
  if (!file) return 0;
  unsigned long long residentPages = 0;
  int fields = fscanf(file, "%*llu %llu", &residentPages);
  fclose(file);
  long pageSize = sysconf(_SC_PAGESIZE);
  if (fields != 1 || pageSize <= 0) return 0;
  u64 pageBytes = (u64)pageSize;
  if ((u64)residentPages > ~0ull / pageBytes) return 0;
  return (u64)residentPages * pageBytes;
#else
  return 0;
#endif
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
  char* dst = (char*)ArenaAlloc(a, s.Length + 1, 1);
  if (s.Length) memcpy(dst, s.Data, s.Length);
  dst[s.Length] = '\0';
  return StrView{dst, s.Length};
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
  DWORD at = GetFileAttributesA(path);
  if (at == INVALID_FILE_ATTRIBUTES) return PathKind::Missing;
  return (at & FILE_ATTRIBUTE_DIRECTORY) ? PathKind::Dir : PathKind::File;
#else
  struct stat st;
  if (stat(path, &st) != 0) return PathKind::Missing;
  return S_ISDIR(st.st_mode) ? PathKind::Dir : PathKind::File;
#endif
}

// Read an entire (small) file into `a`. Empty view on failure. The result is a
// StrBuf so callers can also treat .Data as a C string.
StrView ReadSmallFile(Arena* a, const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return StrView{nullptr, 0};
  StrBuf b = StrBuf_Make(a, 256);
  char buf[512];
  usize n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) StrBuf_Append(&b, StrView{buf, (u32)n});
  fclose(f);
  return StrBuf_View(&b);
}

bool StartsWithAt(StrView s, u32 i, StrView pfx) {
  if (i + pfx.Length > s.Length) return false;
  return memcmp(s.Data + i, pfx.Data, pfx.Length) == 0;
}

char LowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

bool HasHomePrefix(StrView path, StrView home) {
  if (home.Length == 0 || path.Length < home.Length) return false;
  for (u32 i = 0; i < home.Length; i++) {
    char pc = path.Data[i], hc = home.Data[i];
#if defined(_WIN32)
    if (IsSep(pc) && IsSep(hc)) continue;
    if (LowerAscii(pc) != LowerAscii(hc)) return false;
#else
    if (pc != hc) return false;
#endif
  }
  return path.Length == home.Length || IsSep(path.Data[home.Length]);
}

}  // namespace

StrView Sys_CurrentDir(Arena* a) {
  char buf[4096];
#if defined(_WIN32)
  if (!_getcwd(buf, (int)sizeof(buf))) return StrView{nullptr, 0};
#else
  if (!getcwd(buf, sizeof(buf))) return StrView{nullptr, 0};
#endif
  return DupCStr(a, StripTrailingSep(SV(buf)));
}

bool Sys_SetCurrentDir(StrView path) {
  if (!path.Data || !path.Length || path.Length >= 4096) return false;
  char native[4096];
  memcpy(native, path.Data, path.Length);
  native[path.Length] = 0;
#if defined(_WIN32)
  return _chdir(native) == 0;
#else
  return chdir(native) == 0;
#endif
}

StrView Sys_HomeDir(Arena* a) {
#if defined(_WIN32)
  const char* h = getenv("USERPROFILE");
#else
  const char* h = getenv("HOME");
#endif
  if (!h || !*h) return StrView{nullptr, 0};
  return DupCStr(a, StripTrailingSep(SV(h)));
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

StrView Sys_GitBranch(Arena* a, StrView dir) {
  StrView cur = StripTrailingSep(dir);
  for (u32 guard = 0; guard < 260 && cur.Length > 0; guard++) {
    StrBuf dot = JoinPath(a, cur, SVL(".git"));
    PathKind kind = StatKind(StrBuf_CStr(&dot));

    if (kind != PathKind::Missing) {
      StrView gitdirView;
      StrBuf relJoin;  // backing storage for the relative-gitdir case
      if (kind == PathKind::Dir) {
        gitdirView = StrBuf_View(&dot);
      } else {
        StrView gp = Git_GitdirFromFile(ReadSmallFile(a, StrBuf_CStr(&dot)));
        if (gp.Length == 0) return StrView{nullptr, 0};
        if (IsAbsPath(gp)) {
          gitdirView = gp;
        } else {
          relJoin = JoinPath(a, cur, gp);
          gitdirView = StrBuf_View(&relJoin);
        }
      }
      StrBuf head = JoinPath(a, gitdirView, SVL("HEAD"));
      StrView br = Git_BranchFromHead(ReadSmallFile(a, StrBuf_CStr(&head)));
      return br.Length ? DupCStr(a, br) : StrView{nullptr, 0};
    }

    StrView parent = ParentDir(cur);
    if (parent.Length >= cur.Length) break;  // no progress -> at the root
    cur = parent;
  }
  return StrView{nullptr, 0};
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