#include "tools/fs.h"

#include "agent/tool.h"
#include "base/file_io.h"
#include "base/thread.h"
#include "base/win32_utf8.h"
#include "tools/glob.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
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

#if !defined(_WIN32)
static void Fs_CopyPathToStack(StrView path, char* buf, usize cap) {
  FFI(path.Length >= cap);
  if (path.Length) memcpy(buf, path.Data, path.Length);
  buf[path.Length] = '\0';
}
#endif

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
  if (path.Length >= 4096) return r;
#if defined(_WIN32)
  wchar_t buf[4096];
  if (!Win32_Path(path, buf, 4096)) return r;
  WIN32_FILE_ATTRIBUTE_DATA data = {};
  if (GetFileAttributesExW(buf, GetFileExInfoStandard, &data)) {
    ULARGE_INTEGER sz;
    sz.LowPart = data.nFileSizeLow;
    sz.HighPart = data.nFileSizeHigh;
    r.Exists = true;
    r.IsDir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    r.Size = sz.QuadPart;
    r.IsRegular = !r.IsDir && !(data.dwFileAttributes & FILE_ATTRIBUTE_DEVICE);
    if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
      HANDLE h = CreateFileW(buf, FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
      BY_HANDLE_FILE_INFORMATION info = {};
      if (h == INVALID_HANDLE_VALUE) return FsStat{};
      bool valid = GetFileInformationByHandle(h, &info) != 0;
      if (valid) {
        r.IsDir = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        r.IsRegular = !r.IsDir && GetFileType(h) == FILE_TYPE_DISK;
        r.Size = ((u64)info.nFileSizeHigh << 32) | info.nFileSizeLow;
      }
      CloseHandle(h);
      if (!valid) return FsStat{};
    }
  }
#else
  char buf[4096];
  Fs_CopyPathToStack(path, buf, sizeof(buf));
  struct stat st;
  if (stat(buf, &st) == 0) {
    r.Exists = true;
    r.IsDir = S_ISDIR(st.st_mode) != 0;
    r.Size = st.st_size > 0 ? (u64)st.st_size : 0;
    r.IsRegular = S_ISREG(st.st_mode) != 0;
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
  wchar_t native[4096];
  if (!Win32_Path(SV(path), native, 4096)) return false;
  if (_wmkdir(native) == 0) return true;
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

#if defined(_WIN32)
static StrView Fs_DosPath(Arena* arena, StrView path) {
  if (StrView_StartsWith(path, SVL("\\\\?\\UNC\\"))) {
    StrBuf normalized = StrBuf_Make(arena, path.Length);
    StrBuf_AppendCStr(&normalized, "\\\\");
    StrBuf_Append(&normalized, StrView{path.Data + 8, path.Length - 8});
    return StrBuf_View(&normalized);
  }
  if (path.Length >= 7 && StrView_StartsWith(path, SVL("\\\\?\\")) &&
      path.Data[5] == ':') return StrView{path.Data + 4, path.Length - 4};
  return path;
}
#endif

StrView Fs_RealPath(Arena* arena, StrView path) {
#if defined(_WIN32)
  wchar_t native[4096], resolved[4096];
  if (Win32_Path(path, native, 4096)) {
    DWORD n = GetFullPathNameW(native, 4096, resolved, nullptr);
    StrView out = {};
    if (n && n < 4096 && Win32_Utf8(arena, resolved, n, &out)) return Fs_DosPath(arena, out);
  }
#else
  const char* in = StrView_DupCStr(arena, path);
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
  // GetFullPathNameW also resolves drive-relative and rooted paths in the
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
  wchar_t native[4096];
  if (!Win32_Path(path, native, 4096)) return StrView{};
  HANDLE handle = CreateFileW(
      native, FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return StrView{};

  DWORD need = GetFinalPathNameByHandleW(handle, nullptr, 0,
                                         FILE_NAME_NORMALIZED);
  if (!need) {
    CloseHandle(handle);
    return StrView{};
  }
  wchar_t* finalPath = ArenaPushArray(arena, wchar_t, (usize)need + 1);
  DWORD length = GetFinalPathNameByHandleW(handle, finalPath, need + 1,
                                            FILE_NAME_NORMALIZED);
  CloseHandle(handle);
  if (!length || length > need) return StrView{};

  StrView result = {};
  if (!Win32_Utf8(arena, finalPath, length, &result)) return StrView{};
  *ok = true;
  return Fs_DosPath(arena, result);
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
  if (slash < 0) return StrView{path.Data, rootLen};
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

static bool Fs_Error(StrBuf* err, const char* operation, StrView path, u32 code = 0) {
  if (err) {
    StrBuf_AppendCStr(err, operation);
    if (path.Length) {
      StrBuf_AppendCStr(err, ": ");
      StrBuf_Append(err, path);
    }
    if (code) {
      StrBuf_AppendCStr(err, " (OS error ");
      StrBuf_AppendU64(err, code);
      StrBuf_AppendChar(err, ')');
    }
  }
  return false;
}

static bool Fs_Cancelled(volatile i32* abort, StrBuf* err) {
  if (!abort || !Atomic_LoadI32(abort)) return false;
  Fs_Error(err, "Filesystem traversal cancelled", StrView{});
  return true;
}

static Array<FsEntry> Fs_ListDirChecked(Arena* arena, StrView dir, bool* ok,
                                       volatile i32* abort, StrBuf* err, usize memoryLimit = 0) {
  Array<FsEntry> entries = Array_Make<FsEntry>(arena, 16);
  *ok = false;
  if (dir.Length > 4090 || Fs_PathHasNul(dir)) {
    Fs_Error(err, "Invalid or overlong directory path", dir);
    return entries;
  }
  if (Fs_Cancelled(abort, err)) return entries;
#if defined(_WIN32)
  StrBuf pattern = StrBuf_Make(arena, dir.Length + 3);
  StrBuf_Append(&pattern, dir);
  if (pattern.Length == 0 || !Fs_IsSep(pattern.Data[pattern.Length - 1])) StrBuf_AppendChar(&pattern, '\\');
  StrBuf_AppendChar(&pattern, '*');
  wchar_t native[4096];
  if (!Win32_Path(StrBuf_View(&pattern), native, 4096)) {
    Fs_Error(err, "Invalid UTF-8 directory path", dir, GetLastError());
    return entries;
  }
  WIN32_FIND_DATAW fd = {};
  HANDLE h = FindFirstFileW(native, &fd);
  if (h == INVALID_HANDLE_VALUE) {
    DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND && Fs_IsDir(dir)) *ok = true;
    else Fs_Error(err, "Cannot list directory", dir, code);
    return entries;
  }
  *ok = true;
  for (;;) {
    if (Fs_Cancelled(abort, err)) { *ok = false; break; }
    StrView name = {};
    if (!Win32_Utf8(arena, fd.cFileName, (u32)lstrlenW(fd.cFileName), &name)) {
      *ok = Fs_Error(err, "Cannot encode directory entry as UTF-8", dir, GetLastError());
      break;
    }
    if (!StrView_Eq(name, SVL(".")) && !StrView_Eq(name, SVL(".."))) {
      ULARGE_INTEGER sz;
      sz.LowPart = fd.nFileSizeLow;
      sz.HighPart = fd.nFileSizeHigh;
      FsEntry e = {};
      e.Name = name;
      e.IsDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
      e.Size = sz.QuadPart;
      e.IsSymlink = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
      e.IsRegular = !e.IsDir && !e.IsSymlink &&
                    !(fd.dwFileAttributes & FILE_ATTRIBUTE_DEVICE);
      Array_Push(&entries, e);
    }
    if (entries.Count >= 1000000 || (memoryLimit && arena->Used > memoryLimit)) {
      *ok = Fs_Error(err, "Directory entry/memory limit exceeded", dir);
      break;
    }
    if (!FindNextFileW(h, &fd)) {
      DWORD code = GetLastError();
      if (code != ERROR_NO_MORE_FILES)
        *ok = Fs_Error(err, "Cannot read directory", dir, code);
      break;
    }
  }
  if (!FindClose(h) && *ok)
    *ok = Fs_Error(err, "Cannot close directory", dir, GetLastError());
#else
  DIR* d = opendir(StrView_DupCStr(arena, dir));
  if (!d) {
    Fs_Error(err, "Cannot list directory", dir, (u32)errno);
    return entries;
  }
  *ok = true;
  for (;;) {
    if (Fs_Cancelled(abort, err)) { *ok = false; break; }
    errno = 0;
    struct dirent* de = readdir(d);
    if (!de) {
      if (errno) *ok = Fs_Error(err, "Cannot read directory", dir, (u32)errno);
      break;
    }
    StrView name = SV(de->d_name);
    if (StrView_Eq(name, SVL(".")) || StrView_Eq(name, SVL(".."))) continue;
    struct stat st = {};
    if (fstatat(dirfd(d), de->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
      u32 code = (u32)errno;
      *ok = Fs_Error(err, "Cannot stat directory entry", Fs_Join(arena, dir, name), code);
      break;
    }
    FsEntry e = {};
    e.Name = StrView_Dup(arena, name);
    e.IsDir = S_ISDIR(st.st_mode) != 0;
    e.Size = st.st_size > 0 ? (u64)st.st_size : 0;
    e.IsSymlink = S_ISLNK(st.st_mode) != 0;
    e.IsRegular = S_ISREG(st.st_mode) != 0;
    Array_Push(&entries, e);
    if (entries.Count >= 1000000 || (memoryLimit && arena->Used > memoryLimit)) {
      *ok = Fs_Error(err, "Directory entry/memory limit exceeded", dir);
      break;
    }
  }
  if (closedir(d) != 0 && *ok)
    *ok = Fs_Error(err, "Cannot close directory", dir, (u32)errno);
#endif
  return entries;
}

Array<FsEntry> Fs_ListDir(Arena* arena, StrView dir, bool* ok) {
  bool listed = false;
  Array<FsEntry> result = Fs_ListDirChecked(arena, dir, &listed, nullptr, nullptr);
  if (ok) *ok = listed;
  return result;
}

static bool Fs_HasSlash(StrView s) {
  for (u32 i = 0; i < s.Length; i++)
    if (s.Data[i] == '/') return true;
  return false;
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

static bool Fs_RelFromBase(StrView base, StrView rel, StrView* out) {
  while (base.Length > 1 && base.Data[base.Length - 1] == '/') base.Length--;
  if (base.Length == 0) {
    *out = rel;
    return true;
  }
  if (rel.Length <= base.Length) return false;
  if (!Fs_PathPrefixEq(rel, base, base.Length)) return false;
  u32 start = base.Length;
  if (base.Data[base.Length - 1] != '/') {
    if (rel.Data[start] != '/') return false;
    start++;
  }
  *out = StrView{rel.Data + start, rel.Length - start};
  return true;
}

static bool Fs_IgnoreMatches(const FsIgnorePattern& p, StrView relPath, bool isDir,
                             bool* matched) {
  *matched = false;
  if (p.DirOnly && !isDir) return true;
  StrView rel = {};
  if (!Fs_RelFromBase(p.BaseRel, relPath, &rel)) return true;
  if (!p.Anchored && !Fs_HasSlash(p.Pattern)) rel = Fs_Base(rel);
  return Glob_MatchChecked(p.Pattern, rel, false, matched);
}

bool Fs_IsIgnored(const Array<FsIgnorePattern>* patterns, StrView relPath, bool isDir) {
  // Test ancestors separately: a file negation cannot reopen a pruned directory.
  for (u32 end = 0; end <= relPath.Length; end++) {
    if (end != relPath.Length && relPath.Data[end] != '/') continue;
    if (!end) continue;
    bool ignored = false;
    for (u32 i = 0; i < patterns->Count; i++) {
      bool matched = false;
      if (Fs_IgnoreMatches(patterns->Data[i], StrView{relPath.Data, end},
                           end < relPath.Length || isDir, &matched) && matched)
        ignored = !patterns->Data[i].Negated;
    }
    if (ignored) return true;
  }
  return false;
}

static StrView Fs_TrimLine(StrView s) {
  if (s.Length && s.Data[s.Length - 1] == '\r') s.Length--;
  while (s.Length && s.Data[s.Length - 1] == ' ') {
    u32 backslashes = 0;
    for (u32 i = s.Length - 1; i && s.Data[i - 1] == '\\'; i--) backslashes++;
    if (backslashes % 2) break;
    s.Length--;
  }
  return s;
}

static bool Fs_ReadIgnoreFile(Arena* arena, StrView path, StrView* content,
                              volatile i32* abort, StrBuf* err) {
  *content = StrView{};
#if defined(_WIN32)
  wchar_t native[4096];
  if (!Win32_Path(path, native, 4096))
    return Fs_Error(err, "Invalid UTF-8 ignore file path", path, GetLastError());
  DWORD attrs = GetFileAttributesW(native);
  if (attrs == INVALID_FILE_ATTRIBUTES) {
    DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return true;
    return Fs_Error(err, "Cannot stat ignore file", path, code);
  }
  if (attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE))
    return Fs_Error(err, "Ignore file is not a regular file", path);
#else
  const char* native = StrView_DupCStr(arena, path);
  struct stat st = {};
  if (lstat(native, &st) != 0) {
    if (errno == ENOENT) return true;
    return Fs_Error(err, "Cannot stat ignore file", path, (u32)errno);
  }
  if (S_ISLNK(st.st_mode) && stat(native, &st) != 0)
    return Fs_Error(err, "Cannot stat ignore file target", path, (u32)errno);
  if (!S_ISREG(st.st_mode)) return Fs_Error(err, "Ignore file is not a regular file", path);
#endif
  FILE* f = nullptr;
  if (File_Open(arena, path, "rb", &f) != FileIoResult::Success)
    return Fs_Error(err, "Cannot open ignore file", path, (u32)errno);
  StrBuf out = StrBuf_Make(arena, 1024);
  char buf[1024];
  bool ok = true;
  for (;;) {
    if (Fs_Cancelled(abort, err)) { ok = false; break; }
    size_t n = fread(buf, 1, sizeof(buf), f);
    if (n > 0) StrBuf_Append(&out, StrView{buf, (u32)n});
    if (ferror(f)) { ok = Fs_Error(err, "Cannot read ignore file", path, (u32)errno); break; }
    if (out.Length > 1024u * 1024u) {
      ok = Fs_Error(err, "Ignore file exceeds 1 MiB limit", path);
      break;
    }
    if (n < sizeof(buf)) break;
  }
  if (fclose(f) != 0 && ok) ok = Fs_Error(err, "Cannot close ignore file", path, (u32)errno);
  if (ok) *content = StrBuf_View(&out);
  return ok;
}

static bool Fs_LoadIgnoreFileChecked(Arena* arena, Array<FsIgnorePattern>* patterns, StrView dirAbs,
                                     StrView dirRel, StrView fileName,
                                     volatile i32* abort, StrBuf* err) {
  StrView path = Fs_Join(arena, dirAbs, fileName);
  StrView content = {};
  if (!Fs_ReadIgnoreFile(arena, path, &content, abort, err)) return false;
  if (!content.Data) return true;
  u32 added = 0;
  u32 start = 0;
  for (u32 i = 0; i <= content.Length; i++) {
    if (i != content.Length && content.Data[i] != '\n') continue;
    StrView line = Fs_TrimLine(StrView{content.Data + start, i - start});
    start = i + 1;
    if (line.Length == 0 || line.Data[0] == '#') continue;
    FsIgnorePattern p = {};
    p.BaseRel = dirRel;
    if (line.Data[0] == '!') {
      p.Negated = true;
      line.Data++;
      line.Length--;
    }
    if (!line.Length) continue;
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
    if (line.Length > 16384 || ++added > 4096)
      return Fs_Error(err, "Ignore rule length/count limit exceeded", path);
    if (Fs_PathHasNul(line)) return Fs_Error(err, "NUL in ignore rule", path);
    p.Pattern = line;
    Array_Push(patterns, p);
  }
  return true;
}

void Fs_LoadIgnoreFile(Arena* arena, Array<FsIgnorePattern>* patterns, StrView dirAbs,
                       StrView dirRel, StrView fileName) {
  Fs_LoadIgnoreFileChecked(arena, patterns, dirAbs, dirRel, fileName, nullptr, nullptr);
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

struct FsIgnoreScope {
  const FsIgnoreScope* Parent;
  Array<FsIgnorePattern> Git;
  Array<FsIgnorePattern> Ignore;
};

static const u32 FS_WALK_MAX_DEPTH = 128;
static const usize FS_WALK_MAX_SCRATCH = 64u * 1024u * 1024u;

static bool Fs_LoadScope(Arena* arena, FsIgnoreScope* scope, StrView absDir,
                         const FsWalkOptions& opts, StrBuf* err) {
  scope->Git.Backing = arena;
  scope->Ignore.Backing = arena;
  // Absolute matching coordinates keep parent-file anchoring intact when the
  // visitor's relative paths start below the repository root.
  StrView base = Fs_ToPosix(arena, absDir);
  if (opts.RespectGitignore &&
      !Fs_LoadIgnoreFileChecked(arena, &scope->Git, absDir, base, SVL(".gitignore"),
                                opts.Abort, err)) return false;
  if (opts.RespectIgnoreFiles &&
      !Fs_LoadIgnoreFileChecked(arena, &scope->Ignore, absDir, base, SVL(".ignore"),
                                opts.Abort, err)) return false;
  return true;
}

static bool Fs_ScopeIgnored(const FsIgnoreScope* scope, StrView path, bool isDir,
                            bool* ignored, StrBuf* err) {
  *ignored = false;
  // .ignore outranks .gitignore across scopes; within each source type, the
  // deepest scope wins and the last matching rule wins within that file.
  for (u32 source = 0; source < 2; source++) {
    for (const FsIgnoreScope* current = scope; current; current = current->Parent) {
      const Array<FsIgnorePattern>* rules = source == 0 ? &current->Ignore : &current->Git;
      for (u32 i = rules->Count; i; i--) {
        const FsIgnorePattern& rule = rules->Data[i - 1];
        bool matched = false;
        if (!Fs_IgnoreMatches(rule, path, isDir, &matched))
          return Fs_Error(err, "Ignore glob complexity limit exceeded", rule.Pattern);
        if (matched) {
          *ignored = !rule.Negated;
          return true;
        }
      }
    }
  }
  return true;
}

struct FsWalkState {
  Arena* Scratch;
  const FsWalkOptions* Options;
  FsWalkVisitor Visit;
  void* User;
  StrBuf* Error;
  bool Stopped;
};

static bool Fs_WalkDir(FsWalkState* state, StrView absDir, StrView relDir,
                       const FsIgnoreScope* inherited, u32 depth);

static bool Fs_WalkDirBody(FsWalkState* state, StrView absDir, StrView relDir,
                           const FsIgnoreScope* inherited, u32 depth) {
  const FsWalkOptions& opts = *state->Options;
  Arena* arena = state->Scratch;
  if (Fs_Cancelled(opts.Abort, state->Error)) return false;
  if (depth > FS_WALK_MAX_DEPTH)
    return Fs_Error(state->Error, "Filesystem traversal depth limit exceeded", absDir);
  if (absDir.Length > 4090)
    return Fs_Error(state->Error, "Filesystem traversal path limit exceeded", absDir);
  if (arena->Used > FS_WALK_MAX_SCRATCH)
    return Fs_Error(state->Error, "Filesystem traversal memory limit exceeded", absDir);
  FsIgnoreScope scope = {};
  scope.Parent = inherited;
  if (!Fs_LoadScope(arena, &scope, absDir, opts, state->Error)) return false;
  bool ok = false;
  Array<FsEntry> entries =
      Fs_ListDirChecked(arena, absDir, &ok, opts.Abort, state->Error, FS_WALK_MAX_SCRATCH);
  if (!ok) return false;
  for (u32 i = 0; i < entries.Count; i++) {
    if (Fs_Cancelled(opts.Abort, state->Error)) return false;
    const FsEntry& e = entries.Data[i];
    if (Fs_PathEquals(e.Name, SVL(".git")) || (opts.SkipLinks && e.IsSymlink)) continue;
    usize mark = arena->Used;
    StrView abs = Fs_Join(arena, absDir, e.Name);
    StrView rel = Fs_AppendRel(arena, relDir, e.Name);
    if (abs.Length >= 4096)
      return Fs_Error(state->Error, "Filesystem traversal path limit exceeded", abs);
    bool ignored = false;
    if (!Fs_ScopeIgnored(&scope, Fs_ToPosix(arena, abs), e.IsDir, &ignored, state->Error))
      return false;
    if (!ignored && opts.Filter) {
      bool include = true;
      if (!opts.Filter(opts.FilterUser, rel, e, &include, state->Error)) return false;
      ignored = !include;
    }
    if (!ignored) {
      if ((!e.IsDir || opts.IncludeDirs) && !state->Visit(state->User, abs, rel, e)) {
        if (Fs_Cancelled(opts.Abort, state->Error)) return false;
        state->Stopped = true;
        return true;
      }
      if (e.IsDir && !e.IsSymlink &&
          !Fs_WalkDir(state, abs, rel, &scope, depth + 1)) return false;
      if (state->Stopped) return true;
    }
    ArenaRestore(arena, mark);
  }
  return !Fs_Cancelled(opts.Abort, state->Error);
}

static bool Fs_WalkDir(FsWalkState* state, StrView absDir, StrView relDir,
                       const FsIgnoreScope* inherited, u32 depth) {
  usize mark = state->Scratch->Used;
  bool ok = Fs_WalkDirBody(state, absDir, relDir, inherited, depth);
  ArenaRestore(state->Scratch, mark);
  return ok;
}

static bool Fs_LoadParentScopes(FsWalkState* state, StrView root,
                                const FsIgnoreScope** inherited, bool* ignored) {
  Arena* arena = state->Scratch;
  *ignored = false;
  Array<StrView> ancestors = Array_Make<StrView>(arena, FS_WALK_MAX_DEPTH);
  StrView current = root;
  for (;;) {
    if (Fs_Cancelled(state->Options->Abort, state->Error)) return false;
    StrView marker = Fs_Join(arena, current, SVL(".git"));
#if defined(_WIN32)
    wchar_t native[4096];
    if (!Win32_Path(marker, native, 4096))
      return Fs_Error(state->Error, "Invalid UTF-8 repository boundary", marker, GetLastError());
    DWORD attrs = GetFileAttributesW(native);
    if (attrs != INVALID_FILE_ATTRIBUTES) break;
    DWORD code = GetLastError();
    if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND)
      return Fs_Error(state->Error, "Cannot inspect repository boundary", marker, code);
#else
    struct stat st = {};
    if (lstat(StrView_DupCStr(arena, marker), &st) == 0) break;
    if (errno != ENOENT)
      return Fs_Error(state->Error, "Cannot inspect repository boundary", marker, (u32)errno);
#endif
    StrView parent = Fs_Parent(current);
    if (!parent.Length || Fs_PathEquals(parent, current)) break;
    if (ancestors.Count >= FS_WALK_MAX_DEPTH)
      return Fs_Error(state->Error, "Parent ignore depth limit exceeded", root);
    Array_Push(&ancestors, parent);
    current = parent;
  }
  for (u32 i = ancestors.Count; i; i--) {
    if (arena->Used > FS_WALK_MAX_SCRATCH)
      return Fs_Error(state->Error, "Parent ignore memory limit exceeded", root);
    FsIgnoreScope* scope = ArenaPushStruct(arena, FsIgnoreScope);
    *scope = FsIgnoreScope{};
    scope->Parent = *inherited;
    if (!Fs_LoadScope(arena, scope, ancestors.Data[i - 1], *state->Options, state->Error))
      return false;
    *inherited = scope;
  }
  // Searching an explicit subdirectory does not let a file negation resurrect
  // a directory excluded by an inherited rule.
  StrView path = Fs_ToPosix(arena, root);
  for (u32 end = 1; end <= path.Length; end++) {
    if (end != path.Length && path.Data[end] != '/') continue;
    if (!Fs_ScopeIgnored(*inherited, StrView{path.Data, end}, true, ignored, state->Error))
      return false;
    if (*ignored) break;
  }
  return true;
}

bool Fs_WalkChecked(Arena* arena, StrView root, const FsWalkOptions& opts,
                    FsWalkVisitor visit, void* user, StrBuf* err) {
  (void)arena;
  if (Fs_Cancelled(opts.Abort, err)) return false;
  if (!visit) return Fs_Error(err, "Missing filesystem visitor", root);
  if (!root.Length || root.Length > 4090 || Fs_PathHasNul(root))
    return Fs_Error(err, "Invalid or overlong traversal root", root);
  // Never restore the caller's arena: visitors allocate retained output there.
  Arena scratch = ArenaCreate();
  StrView absRoot = Fs_RealPath(&scratch, root);
  FsWalkState state = {&scratch, &opts, visit, user, err, false};
  const FsIgnoreScope* inherited = nullptr;
  bool ignored = false;
  FsStat rootStat = Fs_Stat(absRoot);
  bool ok = rootStat.Exists && rootStat.IsDir;
  if (!ok) Fs_Error(err, "Cannot open traversal directory", root);
  if (ok && opts.LoadParentIgnores && (opts.RespectGitignore || opts.RespectIgnoreFiles))
    ok = Fs_LoadParentScopes(&state, absRoot, &inherited, &ignored);
  if (ok && !ignored) ok = Fs_WalkDir(&state, absRoot, StrView{}, inherited, 0);
  if (ok && !state.Stopped && Fs_Cancelled(opts.Abort, err)) ok = false;
  ArenaRelease(&scratch);
  return ok;
}

void Fs_Walk(Arena* arena, StrView root, const FsWalkOptions& opts, FsWalkVisitor visit, void* user) {
  if (visit) Fs_WalkChecked(arena, root, opts, visit, user, nullptr);
}
