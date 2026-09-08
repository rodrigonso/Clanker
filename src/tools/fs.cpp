#include "tools/fs.h"

#include "agent/tool.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

bool Fs_IsSep(char c) { return c == '/' || c == '\\'; }

char Fs_PathFold(char c) {
#if defined(_WIN32)
  c = Ascii_ToLower(c);
#endif
  return Fs_IsSep(c) ? '/' : c;
}

bool Fs_PathEquals(StrView a, StrView b) {
  if (a.Length != b.Length) return false;
  for (u32 i = 0; i < a.Length; i++) {
    if (Fs_PathFold(a.Data[i]) != Fs_PathFold(b.Data[i])) return false;
  }
  return true;
}

static void Fs_CopyPathToStack(StrView path, char* buf, usize cap) {
  FFI(path.Length >= cap);
  if (path.Length) memcpy(buf, path.Data, path.Length);
  buf[path.Length] = '\0';
}

static u32 Fs_RootLength(StrView path) {
  if (path.Length == 0) return 0;
#if defined(_WIN32)
  if (path.Length >= 2 && path.Data[1] == ':') {
    if (path.Length >= 3 && Fs_IsSep(path.Data[2])) return 3;
    return 2;
  }
  if (path.Length >= 2 && Fs_IsSep(path.Data[0]) && Fs_IsSep(path.Data[1])) {
    u32 parts = 0;
    for (u32 i = 2; i < path.Length; i++) {
      if (Fs_IsSep(path.Data[i])) {
        parts++;
        while (i + 1 < path.Length && Fs_IsSep(path.Data[i + 1])) i++;
        if (parts == 2) return i + 1;
      }
    }
    return path.Length;
  }
#endif
  return Fs_IsSep(path.Data[0]) ? 1u : 0u;
}

static bool Fs_PathIsAbsolute(StrView path) {
  if (path.Length == 0) return false;
  if (Fs_IsSep(path.Data[0])) return true;
#if defined(_WIN32)
  return path.Length >= 3 && path.Data[1] == ':' && Fs_IsSep(path.Data[2]);
#else
  return false;
#endif
}

static StrView Fs_NormalizedCopy(Arena* arena, StrView path) {
  StrBuf out = StrBuf_Make(arena, path.Length + 1);
  bool lastSep = false;
  for (u32 i = 0; i < path.Length; i++) {
    char c = path.Data[i];
    bool sep = Fs_IsSep(c);
    if (sep) {
      if (!lastSep) StrBuf_AppendChar(&out, c);
      lastSep = true;
    } else {
      StrBuf_AppendChar(&out, c);
      lastSep = false;
    }
  }
  return StrBuf_View(&out);
}

FsStat Fs_Stat(StrView path) {
  FsStat r = {};
  char buf[4096];
  Fs_CopyPathToStack(path, buf, sizeof(buf));
#if defined(_WIN32)
  WIN32_FILE_ATTRIBUTE_DATA data = {};
  if (GetFileAttributesExA(buf, GetFileExInfoStandard, &data)) {
    ULARGE_INTEGER sz;
    sz.LowPart = data.nFileSizeLow;
    sz.HighPart = data.nFileSizeHigh;
    r.Exists = true;
    r.IsDir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    r.Size = sz.QuadPart;
  }
#else
  struct stat st;
  if (stat(buf, &st) == 0) {
    r.Exists = true;
    r.IsDir = S_ISDIR(st.st_mode) != 0;
    r.Size = st.st_size > 0 ? (u64)st.st_size : 0;
  }
#endif
  return r;
}

bool Fs_Exists(StrView path) { return Fs_Stat(path).Exists; }

bool Fs_IsDir(StrView path) {
  FsStat st = Fs_Stat(path);
  return st.Exists && st.IsDir;
}

static bool Fs_MkdirOne(const char* path) {
#if defined(_WIN32)
  if (_mkdir(path) == 0) return true;
#else
  if (mkdir(path, 0700) == 0) return true;
#endif
  if (errno == EEXIST) return Fs_IsDir(SV(path));
  return false;
}

bool Fs_MkdirP(Arena* arena, StrView path) {
  if (path.Length == 0) return false;
  StrBuf p = StrBuf_Make(arena, path.Length + 1);
  StrBuf_Append(&p, path);
  u32 rootLen = Fs_RootLength(StrBuf_View(&p));
  while (p.Length > rootLen && Fs_IsSep(p.Data[p.Length - 1])) p.Data[--p.Length] = '\0';
  if (p.Length <= rootLen) return true;

  for (u32 i = rootLen; i < p.Length; i++) {
    if (!Fs_IsSep(p.Data[i])) continue;
    char old = p.Data[i];
    p.Data[i] = '\0';
    if (i > rootLen && !Fs_MkdirOne(p.Data)) return false;
    p.Data[i] = old;
  }
  return Fs_MkdirOne(p.Data);
}

StrView Fs_RealPath(Arena* arena, StrView path) {
  const char* in = StrView_DupCStr(arena, path);
#if defined(_WIN32)
  DWORD need = GetFullPathNameA(in, 0, nullptr, nullptr);
  if (need) {
    char* out = (char*)ArenaAlloc(arena, (usize)need + 1, 1);
    DWORD n = GetFullPathNameA(in, need + 1, out, nullptr);
    if (n && n < need + 1) return StrView{out, (u32)n};
  }
#else
  char resolved[4096];
  if (realpath(in, resolved)) return StrView_Dup(arena, SV(resolved));
#endif
  return Fs_NormalizedCopy(arena, path);
}

static bool Fs_IsDriveRelative(StrView path) {
#if defined(_WIN32)
  return path.Length >= 2 && path.Data[1] == ':' &&
         !(path.Length >= 3 && Fs_IsSep(path.Data[2]));
#else
  (void)path;
  return false;
#endif
}

static StrView Fs_AbsoluteForPermission(Arena* arena, StrView path,
                                        bool* safe) {
  if (path.Length == 0) {
    *safe = false;
    return StrView{};
  }
  for (u32 i = 0; i < path.Length; i++) {
    if (path.Data[i] == '\0') *safe = false;
  }
#if defined(_WIN32)
  // GetFullPathNameA also resolves drive-relative and rooted paths in the
  // same way the process will interpret them. The former remains unsafe for
  // workspace classification and is rejected by the caller.
  return Fs_RealPath(arena, path);
#else
  if (Fs_PathIsAbsolute(path)) return StrView_Dup(arena, path);
  char cwd[4096];
  if (!getcwd(cwd, sizeof(cwd))) {
    *safe = false;
    return StrView_Dup(arena, path);
  }
  return Fs_Join(arena, SV(cwd), path);
#endif
}

static StrView Fs_CanonicalExisting(Arena* arena, StrView path, bool* ok) {
  *ok = false;
#if defined(_WIN32)
  const char* native = StrView_DupCStr(arena, path);
  HANDLE handle = CreateFileA(
      native, FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return StrView{};

  DWORD need = GetFinalPathNameByHandleA(handle, nullptr, 0,
                                         FILE_NAME_NORMALIZED);
  if (!need) {
    CloseHandle(handle);
    return StrView{};
  }
  char* finalPath = (char*)ArenaAlloc(arena, (usize)need + 1, 1);
  DWORD length = GetFinalPathNameByHandleA(handle, finalPath, need + 1,
                                            FILE_NAME_NORMALIZED);
  CloseHandle(handle);
  if (!length || length > need) return StrView{};

  StrView result{finalPath, length};
  if (result.Length >= 8 && result.Data[0] == '\\' &&
      result.Data[1] == '\\' && result.Data[2] == '?' &&
      result.Data[3] == '\\' && result.Data[4] == 'U' &&
      result.Data[5] == 'N' && result.Data[6] == 'C' &&
      result.Data[7] == '\\') {
    StrBuf normalized = StrBuf_Make(arena, result.Length);
    StrBuf_AppendCStr(&normalized, "\\\\");
    StrBuf_Append(&normalized, StrView{result.Data + 8, result.Length - 8});
    result = StrBuf_View(&normalized);
  } else if (result.Length >= 7 && result.Data[0] == '\\' &&
             result.Data[1] == '\\' && result.Data[2] == '?' &&
             result.Data[3] == '\\' && result.Data[5] == ':') {
    result = StrView{result.Data + 4, result.Length - 4};
  }
  *ok = true;
  return result;
#else
  char resolved[4096];
  const char* native = StrView_DupCStr(arena, path);
  if (!realpath(native, resolved)) return StrView{};
  *ok = true;
  return StrView_Dup(arena, SV(resolved));
#endif
}

static bool Fs_NextSegment(StrView s, u32* pos, StrView* seg);

static bool Fs_PathHasNul(StrView path) {
  for (u32 i = 0; i < path.Length; i++)
    if (path.Data[i] == '\0') return true;
  return false;
}

static void Fs_PopPermissionComponent(StrBuf* path) {
  u32 rootLength = Fs_RootLength(StrBuf_View(path));
  u32 end = path->Length;
  while (end > rootLength && Fs_IsSep(path->Data[end - 1])) end--;
  if (end <= rootLength) {
    path->Length = rootLength;
    path->Data[path->Length] = '\0';
    return;
  }
  u32 slash = end;
  while (slash > rootLength && !Fs_IsSep(path->Data[slash - 1])) slash--;
  if (slash <= rootLength) {
    path->Length = rootLength;
  } else {
    path->Length = slash;
  }
  path->Data[path->Length] = '\0';
}

static bool Fs_IsDot(StrView segment) {
  return StrView_EqCStr(segment, ".");
}

static bool Fs_IsDotDot(StrView segment) {
  return StrView_EqCStr(segment, "..");
}

bool Fs_PathContained(StrView root, StrView path) {
  if (root.Length == 0 || path.Length == 0) return false;
  u32 rootLength = Fs_RootLength(root);
  u32 rootEnd = root.Length;
  while (rootEnd > rootLength && Fs_IsSep(root.Data[rootEnd - 1])) rootEnd--;
  if (rootEnd == rootLength && rootEnd > 1 &&
      Fs_IsSep(root.Data[rootEnd - 1]))
    rootEnd--;
  if (rootEnd == 0) return false;
  if (rootEnd == 1 && Fs_IsSep(root.Data[0]))
    return path.Length > 0 && Fs_IsSep(path.Data[0]);
  if (path.Length < rootEnd) return false;
  for (u32 i = 0; i < rootEnd; i++) {
    if (Fs_PathFold(root.Data[i]) != Fs_PathFold(path.Data[i])) return false;
  }
  if (path.Length == rootEnd)
    return !(rootEnd < root.Length && rootEnd <= rootLength);
  return Fs_IsSep(path.Data[rootEnd]);
}

struct FsCanonicalPermissionPath {
  StrView Path;
  bool Complete;
  bool Safe;
};

static FsCanonicalPermissionPath Fs_CanonicalizePermissionAbsolute(
    Arena* arena, StrView absolute, bool safeInput) {
  FsCanonicalPermissionPath result = {};
  if (absolute.Length == 0) return result;

  StrBuf current = StrBuf_Make(arena, absolute.Length + 16);
  u32 rootLength = Fs_RootLength(absolute);
  for (u32 i = 0; i < rootLength && i < absolute.Length; i++) {
    char c = absolute.Data[i];
    StrBuf_AppendChar(&current, Fs_IsSep(c) ? '/' : c);
  }

  bool complete = true;
  bool safe = safeInput;
  bool unresolved = false;
  u32 pos = rootLength;
  StrView segment = {};
  while (Fs_NextSegment(absolute, &pos, &segment)) {
    if (Fs_IsDot(segment)) continue;
    if (Fs_IsDotDot(segment)) {
      if (unresolved) safe = false;
      Fs_PopPermissionComponent(&current);
      continue;
    }

    if (unresolved) {
      StrView next = Fs_Join(arena, StrBuf_View(&current), segment);
      StrBuf_Clear(&current);
      StrBuf_Append(&current, next);
      continue;
    }

    StrView candidate = Fs_Join(arena, StrBuf_View(&current), segment);
    if (candidate.Length >= 4096 || Fs_PathHasNul(candidate)) {
      unresolved = true;
      complete = false;
      safe = false;
      StrBuf_Clear(&current);
      StrBuf_Append(&current, candidate);
      continue;
    }
    FsStat stat = Fs_Stat(candidate);
    if (!stat.Exists) {
      unresolved = true;
      complete = false;
      StrBuf_Clear(&current);
      StrBuf_Append(&current, candidate);
      continue;
    }

    bool existingOk = false;
    StrView canonical = Fs_CanonicalExisting(arena, candidate, &existingOk);
    if (!existingOk || canonical.Length == 0) {
      unresolved = true;
      complete = false;
      safe = false;
      StrBuf_Clear(&current);
      StrBuf_Append(&current, candidate);
      continue;
    }
    StrBuf_Clear(&current);
    StrBuf_Append(&current, canonical);
  }

  if (absolute.Length == rootLength) {
    bool existingOk = false;
    StrView canonical = Fs_CanonicalExisting(arena, absolute, &existingOk);
    if (existingOk && canonical.Length) {
      StrBuf_Clear(&current);
      StrBuf_Append(&current, canonical);
    } else {
      complete = false;
      safe = false;
    }
  }

  result.Path = StrBuf_View(&current);
  result.Complete = complete;
  result.Safe = safe;
  return result;
}

FsPermissionTarget Fs_ResolvePermissionTarget(Arena* arena,
                                              StrView workingDirectory,
                                              StrView target) {
  FsPermissionTarget result = {};
  if (!arena || workingDirectory.Length == 0 || target.Length == 0) return result;
  if (workingDirectory.Length > 16384 || target.Length > 16384) return result;

  bool rootSafe = true;
  StrView rootAbsolute =
      Fs_AbsoluteForPermission(arena, workingDirectory, &rootSafe);
  if (!rootAbsolute.Length) return result;

  bool targetSafe = true;
  StrView targetAbsolute = target;
  if (!Fs_PathIsAbsolute(target)) {
    if (Fs_IsDriveRelative(target)) targetSafe = false;
    targetAbsolute = Fs_Join(arena, rootAbsolute, target);
  }
  targetAbsolute =
      Fs_AbsoluteForPermission(arena, targetAbsolute, &targetSafe);
  if (!targetAbsolute.Length) return result;

  FsCanonicalPermissionPath rootPath =
      Fs_CanonicalizePermissionAbsolute(arena, rootAbsolute, rootSafe);
  FsCanonicalPermissionPath targetPath =
      Fs_CanonicalizePermissionAbsolute(arena, targetAbsolute, targetSafe);
  result.Path = targetPath.Path;
  bool safe = rootPath.Safe && targetPath.Safe;
  result.Contained =
      safe && rootPath.Complete && Fs_IsDir(rootPath.Path) &&
      Fs_PathContained(rootPath.Path, targetPath.Path);
  return result;
}

bool Fs_DescribeMutationPermission(Arena* arena, const ToolArgs* args,
                                   StrView workingDirectory,
                                   StrView actionPrefix,
                                   StrView grantPrefix,
                                   ToolPermissionInfo* out, StrBuf* err) {
  StrView path = ToolArgs_GetStr(args, "path");
  if (!path.Length) {
    StrBuf_AppendCStr(err, "missing 'path' argument");
    return false;
  }

  FsPermissionTarget target =
      Fs_ResolvePermissionTarget(arena, workingDirectory, path);
  StrView display = target.Path.Length ? target.Path : path;
  u32 room = actionPrefix.Length < 2048u ? 2048u - actionPrefix.Length : 0;
  u32 displayLength = ClankerMin(display.Length, room);
  if (actionPrefix.Length > 2048u) actionPrefix.Length = 2048u;
  StrBuf action = StrBuf_Make(arena, actionPrefix.Length + displayLength + 1);
  StrBuf_Append(&action, actionPrefix);
  StrBuf_Append(&action, StrView{display.Data, displayLength});
  out->Action = StrBuf_View(&action);
  if (display.Length + grantPrefix.Length <= 8192u) {
    StrBuf key = StrBuf_Make(arena, grantPrefix.Length + display.Length + 1);
    StrBuf_Append(&key, grantPrefix);
    StrBuf_Append(&key, display);
    out->GrantKey = StrBuf_View(&key);
  }
  out->AutoAllow = target.Contained;
  return true;
}

StrView Fs_Parent(StrView path) {
  u32 rootLen = Fs_RootLength(path);
  u32 end = path.Length;
  while (end > rootLen && Fs_IsSep(path.Data[end - 1])) end--;
  if (end <= rootLen) return StrView{"", 0};
  i32 slash = -1;
  for (u32 i = rootLen; i < end; i++) {
    if (Fs_IsSep(path.Data[i])) slash = (i32)i;
  }
  if (slash < 0) return StrView{"", 0};
  u32 parentEnd = (u32)slash;
  while (parentEnd > rootLen && Fs_IsSep(path.Data[parentEnd - 1])) parentEnd--;
  if (parentEnd == 0 && rootLen == 1) parentEnd = 1;
  return StrView{path.Data, parentEnd};
}

StrView Fs_Base(StrView path) {
  u32 rootLen = Fs_RootLength(path);
  u32 end = path.Length;
  while (end > rootLen && Fs_IsSep(path.Data[end - 1])) end--;
  u32 start = rootLen;
  for (u32 i = rootLen; i < end; i++) {
    if (Fs_IsSep(path.Data[i])) start = i + 1;
  }
  return StrView{path.Data + start, end - start};
}

StrView Fs_Ext(StrView path) {
  StrView base = Fs_Base(path);
  i32 dot = -1;
  for (u32 i = 0; i < base.Length; i++) {
    if (base.Data[i] == '.') dot = (i32)i;
  }
  if (dot <= 0) return StrView{"", 0};
  return StrView{base.Data + dot, base.Length - (u32)dot};
}

StrView Fs_Join(Arena* arena, StrView a, StrView b) {
  if (a.Length == 0 || Fs_PathIsAbsolute(b)) return StrView_Dup(arena, b);
  if (b.Length == 0) return StrView_Dup(arena, a);
  StrBuf out = StrBuf_Make(arena, a.Length + b.Length + 2);
  StrBuf_Append(&out, a);
  if (!Fs_IsSep(a.Data[a.Length - 1])) {
#if defined(_WIN32)
    StrBuf_AppendChar(&out, '\\');
#else
    StrBuf_AppendChar(&out, '/');
#endif
  }
  StrBuf_Append(&out, b);
  return StrBuf_View(&out);
}

StrView Fs_ToPosix(Arena* arena, StrView path) {
  StrBuf out = StrBuf_Make(arena, path.Length + 1);
  for (u32 i = 0; i < path.Length; i++) {
    char c = path.Data[i] == '\\' ? '/' : path.Data[i];
    StrBuf_AppendChar(&out, c);
  }
  return StrBuf_View(&out);
}

static u32 Fs_TrimTrailingSlashLength(StrView s) {
  u32 rootLen = Fs_RootLength(s);
  u32 n = s.Length;
  while (n > rootLen && n > 0 && s.Data[n - 1] == '/') n--;
  return n;
}

static bool Fs_PathPrefixEq(StrView a, StrView b, u32 n) {
  if (a.Length < n || b.Length < n) return false;
#if defined(_WIN32)
  return StrView_EqInsensitiveAscii(StrView{a.Data, n}, StrView{b.Data, n});
#else
  return StrView_Eq(StrView{a.Data, n}, StrView{b.Data, n});
#endif
}

StrView Fs_Relativize(Arena* arena, StrView root, StrView path) {
  StrView r = Fs_ToPosix(arena, root);
  StrView p = Fs_ToPosix(arena, path);
  u32 rn = Fs_TrimTrailingSlashLength(r);
  if (p.Length == rn && Fs_PathPrefixEq(p, r, rn)) return SVL(".");
  if (p.Length > rn && Fs_PathPrefixEq(p, r, rn) && (rn == 0 || p.Data[rn] == '/')) {
    u32 start = rn;
    while (start < p.Length && p.Data[start] == '/') start++;
    return StrView{p.Data + start, p.Length - start};
  }
  return p;
}

Array<FsEntry> Fs_ListDir(Arena* arena, StrView dir, bool* ok) {
  Array<FsEntry> entries = Array_Make<FsEntry>(arena, 16);
  if (ok) *ok = false;
#if defined(_WIN32)
  StrBuf pattern = StrBuf_Make(arena, dir.Length + 3);
  StrBuf_Append(&pattern, dir);
  if (pattern.Length == 0 || !Fs_IsSep(pattern.Data[pattern.Length - 1])) StrBuf_AppendChar(&pattern, '\\');
  StrBuf_AppendChar(&pattern, '*');
  WIN32_FIND_DATAA fd = {};
  HANDLE h = FindFirstFileA(StrBuf_CStr(&pattern), &fd);
  if (h == INVALID_HANDLE_VALUE) return entries;
  if (ok) *ok = true;
  for (;;) {
    StrView name = SV(fd.cFileName);
    if (!StrView_Eq(name, SVL(".")) && !StrView_Eq(name, SVL(".."))) {
      ULARGE_INTEGER sz;
      sz.LowPart = fd.nFileSizeLow;
      sz.HighPart = fd.nFileSizeHigh;
      FsEntry e = {};
      e.Name = StrView_Dup(arena, name);
      e.IsDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
      e.Size = sz.QuadPart;
      Array_Push(&entries, e);
    }
    if (!FindNextFileA(h, &fd)) break;
  }
  FindClose(h);
#else
  DIR* d = opendir(StrView_DupCStr(arena, dir));
  if (!d) return entries;
  if (ok) *ok = true;
  for (;;) {
    struct dirent* de = readdir(d);
    if (!de) break;
    StrView name = SV(de->d_name);
    if (StrView_Eq(name, SVL(".")) || StrView_Eq(name, SVL(".."))) continue;
    StrView full = Fs_Join(arena, dir, name);
    FsStat st = Fs_Stat(full);
    FsEntry e = {};
    e.Name = StrView_Dup(arena, name);
    e.IsDir = st.IsDir;
    e.Size = st.Size;
    Array_Push(&entries, e);
  }
  closedir(d);
#endif
  return entries;
}

static bool Fs_HasSlash(StrView s) {
  for (u32 i = 0; i < s.Length; i++)
    if (s.Data[i] == '/') return true;
  return false;
}

static bool Fs_MatchSegment(StrView pat, StrView text) {
  u32 p = 0;
  u32 t = 0;
  u32 star = (u32)-1;
  u32 match = 0;
  while (t < text.Length) {
    if (p < pat.Length && pat.Data[p] == '*') {
      star = p++;
      match = t;
    } else if (p < pat.Length && pat.Data[p] == text.Data[t]) {
      p++;
      t++;
    } else if (star != (u32)-1) {
      p = star + 1;
      t = ++match;
    } else {
      return false;
    }
  }
  while (p < pat.Length && pat.Data[p] == '*') p++;
  return p == pat.Length;
}

static bool Fs_NextSegment(StrView s, u32* pos, StrView* seg) {
  if (*pos > s.Length) return false;
  u32 start = *pos;
  while (start < s.Length && s.Data[start] == '/') start++;
  if (start >= s.Length) {
    *pos = s.Length + 1;
    return false;
  }
  u32 end = start;
  while (end < s.Length && s.Data[end] != '/') end++;
  *seg = StrView{s.Data + start, end - start};
  *pos = end + 1;
  return true;
}

static bool Fs_MatchPathExact(StrView pat, StrView path) {
  u32 pp = 0;
  u32 tp = 0;
  StrView ps = {};
  StrView ts = {};
  for (;;) {
    bool ph = Fs_NextSegment(pat, &pp, &ps);
    bool th = Fs_NextSegment(path, &tp, &ts);
    if (!ph || !th) return ph == th;
    if (!Fs_MatchSegment(ps, ts)) return false;
  }
}

static bool Fs_MatchAnySegment(StrView pat, StrView path) {
  u32 pos = 0;
  StrView seg = {};
  while (Fs_NextSegment(path, &pos, &seg)) {
    if (Fs_MatchSegment(pat, seg)) return true;
  }
  return false;
}

static bool Fs_MatchPathSuffix(StrView pat, StrView path) {
  if (Fs_MatchPathExact(pat, path)) return true;
  for (u32 i = 0; i < path.Length; i++) {
    if (path.Data[i] == '/' && i + 1 < path.Length) {
      StrView suffix = StrView{path.Data + i + 1, path.Length - i - 1};
      if (Fs_MatchPathExact(pat, suffix)) return true;
    }
  }
  return false;
}

static bool Fs_RelFromBase(StrView base, StrView rel, StrView* out) {
  if (base.Length == 0) {
    *out = rel;
    return true;
  }
  if (rel.Length <= base.Length) return false;
  if (!Fs_PathPrefixEq(rel, base, base.Length)) return false;
  if (rel.Data[base.Length] != '/') return false;
  *out = StrView{rel.Data + base.Length + 1, rel.Length - base.Length - 1};
  return true;
}

static bool Fs_IgnoreMatches(const FsIgnorePattern& p, StrView relPath, bool isDir) {
  if (p.DirOnly && !isDir) return false;
  StrView rel = {};
  if (!Fs_RelFromBase(p.BaseRel, relPath, &rel)) return false;
  bool hasSlash = Fs_HasSlash(p.Pattern);
  if (p.Anchored) {
    if (hasSlash) return Fs_MatchPathExact(p.Pattern, rel);
    if (Fs_HasSlash(rel)) return false;
    return Fs_MatchSegment(p.Pattern, rel);
  }
  if (hasSlash) return Fs_MatchPathSuffix(p.Pattern, rel);
  return Fs_MatchAnySegment(p.Pattern, rel);
}

bool Fs_IsIgnored(const Array<FsIgnorePattern>* patterns, StrView relPath, bool isDir) {
  for (u32 i = 0; i < patterns->Count; i++) {
    if (Fs_IgnoreMatches(patterns->Data[i], relPath, isDir)) return true;
  }
  return false;
}

static StrView Fs_TrimLine(StrView s) {
  while (s.Length && (s.Data[0] == ' ' || s.Data[0] == '\t')) {
    s.Data++;
    s.Length--;
  }
  while (s.Length) {
    char c = s.Data[s.Length - 1];
    if (c != ' ' && c != '\t' && c != '\r') break;
    s.Length--;
  }
  return s;
}

static StrView Fs_ReadSmallFile(Arena* arena, StrView path) {
  FILE* f = fopen(StrView_DupCStr(arena, path), "rb");
  if (!f) return StrView{nullptr, 0};
  StrBuf out = StrBuf_Make(arena, 1024);
  char buf[1024];
  for (;;) {
    size_t n = fread(buf, 1, sizeof(buf), f);
    if (n > 0) StrBuf_Append(&out, StrView{buf, (u32)n});
    if (n < sizeof(buf)) break;
    if (out.Length > 64u * 1024u) break;
  }
  fclose(f);
  return StrBuf_View(&out);
}

void Fs_LoadIgnoreFile(Arena* arena, Array<FsIgnorePattern>* patterns, StrView dirAbs,
                       StrView dirRel, StrView fileName) {
  StrView path = Fs_Join(arena, dirAbs, fileName);
  StrView content = Fs_ReadSmallFile(arena, path);
  if (!content.Data) return;
  u32 start = 0;
  for (u32 i = 0; i <= content.Length; i++) {
    if (i != content.Length && content.Data[i] != '\n') continue;
    StrView line = Fs_TrimLine(StrView{content.Data + start, i - start});
    start = i + 1;
    if (line.Length == 0 || line.Data[0] == '#') continue;
    FsIgnorePattern p = {};
    p.BaseRel = dirRel;
    if (line.Data[0] == '/') {
      p.Anchored = true;
      line.Data++;
      line.Length--;
    }
    if (line.Length && line.Data[line.Length - 1] == '/') {
      p.DirOnly = true;
      line.Length--;
    }
    if (line.Length == 0) continue;
    p.Pattern = StrView_Dup(arena, line);
    Array_Push(patterns, p);
  }
}

static StrView Fs_AppendRel(Arena* arena, StrView parent, StrView name) {
  StrView n = Fs_ToPosix(arena, name);
  if (parent.Length == 0) return n;
  StrBuf out = StrBuf_Make(arena, parent.Length + n.Length + 2);
  StrBuf_Append(&out, parent);
  StrBuf_AppendChar(&out, '/');
  StrBuf_Append(&out, n);
  return StrBuf_View(&out);
}

static bool Fs_WalkDir(Arena* arena, StrView absDir, StrView relDir, const FsWalkOptions& opts,
                       const Array<FsIgnorePattern>* inherited, FsWalkVisitor visit, void* user) {
  Array<FsIgnorePattern> patterns = Array_Make<FsIgnorePattern>(arena, inherited->Count + 4);
  for (u32 i = 0; i < inherited->Count; i++) Array_Push(&patterns, inherited->Data[i]);
  if (opts.RespectGitignore)
    Fs_LoadIgnoreFile(arena, &patterns, absDir, relDir, SVL(".gitignore"));

  bool ok = false;
  Array<FsEntry> entries = Fs_ListDir(arena, absDir, &ok);
  if (!ok) return true;
  for (u32 i = 0; i < entries.Count; i++) {
    FsEntry e = entries.Data[i];
    if (e.IsDir && StrView_Eq(e.Name, SVL(".git"))) continue;
    StrView abs = Fs_Join(arena, absDir, e.Name);
    StrView rel = Fs_AppendRel(arena, relDir, e.Name);
    if (opts.RespectGitignore && Fs_IsIgnored(&patterns, rel, e.IsDir)) continue;
    if (!e.IsDir || opts.IncludeDirs) {
      if (!visit(user, abs, rel, e)) return false;
    }
    if (e.IsDir) {
      if (!Fs_WalkDir(arena, abs, rel, opts, &patterns, visit, user)) return false;
    }
  }
  return true;
}

void Fs_Walk(Arena* arena, StrView root, const FsWalkOptions& opts, FsWalkVisitor visit, void* user) {
  if (!visit) return;
  StrView absRoot = Fs_RealPath(arena, root);
  Array<FsIgnorePattern> patterns = Array_Make<FsIgnorePattern>(arena, 4);
  Fs_WalkDir(arena, absRoot, StrView{"", 0}, opts, &patterns, visit, user);
}
