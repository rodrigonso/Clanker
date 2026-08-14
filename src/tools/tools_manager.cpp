#include "tools/tools_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/sysinfo.h"
#include "base/thread.h"
#include "json/json.h"
#include "net/http.h"
#include "tools/exec.h"
#include "tools/fs.h"

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

#if defined(_WIN32)
volatile LONG g_toolManagerLock;
bool ToolManager_TryLock() {
  return InterlockedCompareExchange(&g_toolManagerLock, 1, 0) == 0;
}
void ToolManager_Unlock() { InterlockedExchange(&g_toolManagerLock, 0); }
u64 ToolManager_ProcessId() { return (u64)GetCurrentProcessId(); }
#else
volatile i32 g_toolManagerLock;
bool ToolManager_TryLock() { return __sync_bool_compare_and_swap(&g_toolManagerLock, 0, 1); }
void ToolManager_Unlock() { __sync_lock_release(&g_toolManagerLock); }
u64 ToolManager_ProcessId() { return (u64)(u32)getpid(); }
#endif

struct ToolManagerScopedLock {
  ToolManagerScopedLock() {
    while (!ToolManager_TryLock()) Sleep_Ms(10);
  }
  ~ToolManagerScopedLock() { ToolManager_Unlock(); }
  ToolManagerScopedLock(const ToolManagerScopedLock&) = delete;
  ToolManagerScopedLock& operator=(const ToolManagerScopedLock&) = delete;
};

bool ToolManager_IsTruthy(const char* value) {
  if (!value || !*value) return false;
  if (strcmp(value, "1") == 0) return true;
  const char* expected[] = {"true", "yes"};
  for (u32 word = 0; word < 2; word++) {
    const char* p = value;
    const char* q = expected[word];
    while (*p && *q) {
      char a = *p >= 'A' && *p <= 'Z' ? (char)(*p - 'A' + 'a') : *p;
      if (a != *q) break;
      p++;
      q++;
    }
    if (!*p && !*q) return true;
  }
  return false;
}

bool ToolManager_IsOffline() {
  return ToolManager_IsTruthy(getenv("CLANK_OFFLINE")) ||
         ToolManager_IsTruthy(getenv("PI_OFFLINE"));
}

bool ToolManager_ExecutableWorks(Arena* arena, StrView path, volatile i32* abort) {
  StrView args[] = {SVL("--version")};
  ExecArgvSink sink = {};
  ExecArgvResult result = Exec_RunArgv(arena, path, args, 1, 10000, abort, sink);
  return result.Spawned && !result.TimedOut && result.ExitCode == 0;
}

#if defined(_WIN32)
bool ToolManager_IsAbsoluteWindowsPath(StrView path) {
  if (path.Length >= 3 && path.Data[1] == ':' &&
      (path.Data[2] == '\\' || path.Data[2] == '/'))
    return true;
  return path.Length >= 2 && (path.Data[0] == '\\' || path.Data[0] == '/') &&
         (path.Data[1] == '\\' || path.Data[1] == '/');
}

StrView ToolManager_FindOnWindowsPath(Arena* arena, StrView executable) {
  const char* rawPath = getenv("PATH");
  if (!rawPath || !*rawPath) return StrView{};
  StrView path = SV(rawPath);
  u32 start = 0;
  for (u32 i = 0; i <= path.Length; i++) {
    if (i != path.Length && path.Data[i] != ';') continue;
    u32 entryStart = start;
    u32 entryEnd = i;
    while (entryStart < entryEnd &&
           (path.Data[entryStart] == ' ' || path.Data[entryStart] == '\t'))
      entryStart++;
    while (entryEnd > entryStart &&
           (path.Data[entryEnd - 1u] == ' ' || path.Data[entryEnd - 1u] == '\t'))
      entryEnd--;
    if (entryEnd > entryStart + 1u && path.Data[entryStart] == '"' &&
        path.Data[entryEnd - 1u] == '"') {
      entryStart++;
      entryEnd--;
    }
    StrView entry{path.Data + entryStart, entryEnd - entryStart};
    if (entry.Length && ToolManager_IsAbsoluteWindowsPath(entry)) {
      StrView candidate = Fs_Join(arena, entry, executable);
      FsStat stat = Fs_Stat(candidate);
      if (stat.Exists && !stat.IsDir) return Fs_RealPath(arena, candidate);
    }
    start = i + 1u;
  }
  return StrView{};
}

StrView ToolManager_SystemExecutable(Arena* arena, StrView relativeSystemPath,
                                     StrView fallbackName) {
  const char* systemRoot = getenv("SystemRoot");
  if (!systemRoot || !*systemRoot) systemRoot = getenv("WINDIR");
  if (systemRoot && *systemRoot) {
    StrView candidate = Fs_Join(arena, SV(systemRoot), relativeSystemPath);
    FsStat stat = Fs_Stat(candidate);
    if (stat.Exists && !stat.IsDir) return Fs_RealPath(arena, candidate);
  }
  return ToolManager_FindOnWindowsPath(arena, fallbackName);
}
#endif

StrView ToolManager_BinDir(Arena* arena) {
  StrView home = Sys_HomeDir(arena);
  StrBuf path = StrBuf_Make(arena, home.Length + 20);
  StrBuf_Append(&path, home.Length ? home : SVL("."));
  StrBuf_AppendCStr(&path, "/.clank/bin");
  return StrBuf_View(&path);
}

StrView ToolManager_LocalRipgrepPath(Arena* arena) {
  StrView dir = ToolManager_BinDir(arena);
#if defined(_WIN32)
  return Fs_Join(arena, dir, SVL("rg.exe"));
#else
  return Fs_Join(arena, dir, SVL("rg"));
#endif
}

bool ToolManager_WriteFile(Arena* arena, StrView path, StrView content) {
  const char* native = StrView_DupCStr(arena, path);
  FILE* file = fopen(native, "wb");
  bool ok = file && (content.Length == 0 ||
                     fwrite(content.Data, 1, content.Length, file) == content.Length);
  if (file && fclose(file) != 0) ok = false;
  return ok;
}

void ToolManager_RemoveTree(Arena* arena, StrView root) {
  bool ok = false;
  Array<FsEntry> entries = Fs_ListDir(arena, root, &ok);
  if (ok) {
    for (u32 i = 0; i < entries.Count; i++) {
      StrView child = Fs_Join(arena, root, entries.Data[i].Name);
      if (entries.Data[i].IsDir)
        ToolManager_RemoveTree(arena, child);
      else {
        const char* path = StrView_DupCStr(arena, child);
        remove(path);
      }
    }
  }
  const char* path = StrView_DupCStr(arena, root);
#if defined(_WIN32)
  _rmdir(path);
#else
  rmdir(path);
#endif
}

struct FindRipgrepState {
  StrView Name;
  StrView Path;
};

bool ToolManager_FindRipgrep(void* user, StrView absPath, StrView, const FsEntry& entry) {
  FindRipgrepState* state = (FindRipgrepState*)user;
  if (!entry.IsDir && StrView_Eq(entry.Name, state->Name)) {
    state->Path = absPath;
    return false;
  }
  return true;
}

StrView ToolManager_AssetName(Arena* arena, StrView version) {
#if defined(__ANDROID__)
  (void)arena;
  (void)version;
  return StrView{};
#else
  StrBuf asset = StrBuf_Make(arena, version.Length + 64);
  StrBuf_AppendCStr(&asset, "ripgrep-");
  StrBuf_Append(&asset, version);
#if defined(_WIN32)
#if defined(_M_ARM64)
  StrBuf_AppendCStr(&asset, "-aarch64-pc-windows-msvc.zip");
#else
  StrBuf_AppendCStr(&asset, "-x86_64-pc-windows-msvc.zip");
#endif
#elif defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__)
  StrBuf_AppendCStr(&asset, "-aarch64-apple-darwin.tar.gz");
#else
  StrBuf_AppendCStr(&asset, "-x86_64-apple-darwin.tar.gz");
#endif
#elif defined(__linux__)
#if defined(__aarch64__)
  StrBuf_AppendCStr(&asset, "-aarch64-unknown-linux-gnu.tar.gz");
#else
  StrBuf_AppendCStr(&asset, "-x86_64-unknown-linux-musl.tar.gz");
#endif
#else
  return StrView{};
#endif
  return StrBuf_View(&asset);
#endif
}

bool ToolManager_Fetch(Arena* arena, StrView url, volatile i32* abort, HttpResponse* out) {
  HttpHeader headers[] = {{SVL("user-agent"), SVL("clank-coding-agent")},
                          {SVL("accept"), SVL("application/vnd.github+json")}};
  HttpRequest request = {};
  request.Method = SVL("GET");
  request.Url = url;
  request.Headers = headers;
  request.HeaderCount = 2;
  request.FollowRedirects = true;
  *out = Http_Fetch(arena, &request, abort);
  return out->Status >= 200 && out->Status < 300;
}

void ToolManager_AppendFetchError(StrBuf* err, const char* prefix, const HttpResponse& response) {
  StrBuf_AppendCStr(err, prefix);
  if (response.Status < 0) {
    StrBuf_Append(err, response.Error.Length ? response.Error : SVL("network request failed"));
  } else {
    StrBuf_AppendCStr(err, "HTTP ");
    StrBuf_AppendI64(err, response.Status);
  }
}

StrView ToolManager_DownloadRipgrep(Arena* arena, volatile i32* abort, StrBuf* err) {
  Http_GlobalInit();
  HttpResponse release = {};
  if (!ToolManager_Fetch(
          arena, SVL("https://api.github.com/repos/BurntSushi/ripgrep/releases/latest"), abort,
          &release)) {
    ToolManager_AppendFetchError(err, "failed to find the current ripgrep release: ", release);
    return StrView{};
  }

  JsonValue* releaseJson = Json_Parse(arena, release.Body);
  StrView version = Json_GetStr(releaseJson, "tag_name");
  if (version.Length && version.Data[0] == 'v') {
    version.Data++;
    version.Length--;
  }
  if (version.Length == 0) {
    StrBuf_AppendCStr(err, "failed to parse the current ripgrep release");
    return StrView{};
  }

  StrView assetName = ToolManager_AssetName(arena, version);
  if (assetName.Length == 0) {
    StrBuf_AppendCStr(err, "ripgrep downloads are not supported on this platform");
    return StrView{};
  }

  StrBuf url = StrBuf_Make(arena, version.Length + assetName.Length + 80);
  StrBuf_AppendCStr(&url, "https://github.com/BurntSushi/ripgrep/releases/download/");
  StrBuf_Append(&url, version);
  StrBuf_AppendChar(&url, '/');
  StrBuf_Append(&url, assetName);

  HttpResponse archive = {};
  if (!ToolManager_Fetch(arena, StrBuf_View(&url), abort, &archive)) {
    ToolManager_AppendFetchError(err, "failed to download ripgrep: ", archive);
    return StrView{};
  }
  if (abort && Atomic_LoadI32(abort)) {
    StrBuf_AppendCStr(err, "Operation aborted");
    return StrView{};
  }

  StrView binDir = ToolManager_BinDir(arena);
  if (!Fs_MkdirP(arena, binDir)) {
    StrBuf_AppendCStr(err, "failed to create ripgrep tools directory: ");
    StrBuf_Append(err, binDir);
    return StrView{};
  }

  StrView archivePath = Fs_Join(arena, binDir, assetName);
  StrBuf extractName = StrBuf_Make(arena, 64);
  StrBuf_AppendCStr(&extractName, "extract_tmp_rg_");
  StrBuf_AppendU64(&extractName, ToolManager_ProcessId());
  StrBuf_AppendChar(&extractName, '_');
  StrBuf_AppendU64(&extractName, MonotonicMillis());
  StrView extractDir = Fs_Join(arena, binDir, StrBuf_View(&extractName));
  if (!Fs_MkdirP(arena, extractDir) ||
      !ToolManager_WriteFile(arena, archivePath, archive.Body)) {
    StrBuf_AppendCStr(err, "failed to stage the ripgrep download");
    const char* archiveNative = StrView_DupCStr(arena, archivePath);
    remove(archiveNative);
    ToolManager_RemoveTree(arena, extractDir);
    return StrView{};
  }

  StrView extractArgs[5];
  u32 extractArgCount = 0;
  StrView extractor;
#if defined(_WIN32)
  extractor =
      ToolManager_SystemExecutable(arena, SVL("System32\\tar.exe"), SVL("tar.exe"));
  extractArgs[extractArgCount++] = SVL("xf");
#else
  extractor = SVL("tar");
  extractArgs[extractArgCount++] = SVL("xzf");
#endif
  extractArgs[extractArgCount++] = archivePath;
  extractArgs[extractArgCount++] = SVL("-C");
  extractArgs[extractArgCount++] = extractDir;

  StrBuf extractStderr = StrBuf_Make(arena, 512);
  ExecArgvSink extractSink = {};
  extractSink.User = &extractStderr;
  extractSink.OnStderr = [](void* user, StrView chunk) {
    StrBuf_Append((StrBuf*)user, chunk);
    return true;
  };
  ExecArgvResult extracted =
      Exec_RunArgv(arena, extractor, extractArgs, extractArgCount, 120000, abort, extractSink);
#if defined(_WIN32)
  if (!extracted.Spawned || extracted.ExitCode != 0) {
    StrBuf_Clear(&extractStderr);
    StrView fallbackArgs[] = {
        SVL("-NoLogo"),
        SVL("-NoProfile"),
        SVL("-NonInteractive"),
        SVL("-ExecutionPolicy"),
        SVL("Bypass"),
        SVL("-Command"),
        SVL("& { param($archive, $destination) $ErrorActionPreference = 'Stop'; "
            "Expand-Archive -LiteralPath $archive -DestinationPath $destination -Force }"),
        archivePath,
        extractDir,
    };
    StrView powershell = ToolManager_SystemExecutable(
        arena, SVL("System32\\WindowsPowerShell\\v1.0\\powershell.exe"),
        SVL("powershell.exe"));
    if (powershell.Length)
      extracted = Exec_RunArgv(arena, powershell, fallbackArgs,
                               sizeof(fallbackArgs) / sizeof(fallbackArgs[0]), 120000, abort,
                               extractSink);
  }
#endif
  if (!extracted.Spawned || extracted.ExitCode != 0) {
    StrBuf_AppendCStr(err, "failed to extract ripgrep");
    if (extractStderr.Length) {
      StrBuf_AppendCStr(err, ": ");
      StrBuf_Append(err, StrBuf_View(&extractStderr));
    }
    const char* archiveNative = StrView_DupCStr(arena, archivePath);
    remove(archiveNative);
    ToolManager_RemoveTree(arena, extractDir);
    return StrView{};
  }

  FindRipgrepState found = {};
#if defined(_WIN32)
  found.Name = SVL("rg.exe");
#else
  found.Name = SVL("rg");
#endif
  FsWalkOptions walk = {};
  walk.IncludeDirs = false;
  Fs_Walk(arena, extractDir, walk, ToolManager_FindRipgrep, &found);
  StrView destination = ToolManager_LocalRipgrepPath(arena);
  bool installed = false;
  if (found.Path.Length) {
    const char* sourceNative = StrView_DupCStr(arena, found.Path);
    const char* destinationNative = StrView_DupCStr(arena, destination);
#if defined(_WIN32)
    installed = MoveFileExA(sourceNative, destinationNative,
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    installed = rename(sourceNative, destinationNative) == 0 &&
                chmod(destinationNative, 0755) == 0;
#endif
  }

  const char* archiveNative = StrView_DupCStr(arena, archivePath);
  remove(archiveNative);
  ToolManager_RemoveTree(arena, extractDir);

  if (!installed || !ToolManager_ExecutableWorks(arena, destination, abort)) {
    const char* destinationNative = StrView_DupCStr(arena, destination);
    remove(destinationNative);
    StrBuf_AppendCStr(err, installed ? "downloaded ripgrep could not be executed"
                                    : "ripgrep binary was not found in the downloaded archive");
    return StrView{};
  }
  return destination;
}

}  // namespace

StrView ToolManager_EnsureRipgrep(Arena* arena, volatile i32* abort, StrBuf* err) {
  const char* overridePath = getenv("CLANK_RG_PATH");
  if (overridePath && *overridePath) {
    StrView path = Fs_RealPath(arena, SV(overridePath));
    if (Fs_Exists(path)) return path;
    StrBuf_AppendCStr(err, "CLANK_RG_PATH does not exist: ");
    StrBuf_Append(err, path);
    return StrView{};
  }

  StrView localPath = ToolManager_LocalRipgrepPath(arena);
  if (Fs_Exists(localPath)) return localPath;
#if defined(_WIN32)
  StrView systemRipgrep = ToolManager_FindOnWindowsPath(arena, SVL("rg.exe"));
  if (systemRipgrep.Length && ToolManager_ExecutableWorks(arena, systemRipgrep, abort))
    return systemRipgrep;
#else
  if (ToolManager_ExecutableWorks(arena, SVL("rg"), abort)) return SVL("rg");
#endif
  if (abort && Atomic_LoadI32(abort)) {
    StrBuf_AppendCStr(err, "Operation aborted");
    return StrView{};
  }
  if (ToolManager_IsOffline()) {
    StrBuf_AppendCStr(err, "ripgrep (rg) is not available and offline mode is enabled");
    return StrView{};
  }
#if defined(__ANDROID__)
  StrBuf_AppendCStr(err, "ripgrep (rg) is not available; install it with: pkg install ripgrep");
  return StrView{};
#endif

  ToolManagerScopedLock lock;
  localPath = ToolManager_LocalRipgrepPath(arena);
  if (Fs_Exists(localPath)) return localPath;
  StrView downloaded = ToolManager_DownloadRipgrep(arena, abort, err);
  if (downloaded.Length == 0 && err->Length == 0)
    StrBuf_AppendCStr(err, "ripgrep (rg) is not available and could not be downloaded");
  return downloaded;
}
