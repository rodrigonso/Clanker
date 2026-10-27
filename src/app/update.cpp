#include "app/update.h"

#include <stdio.h>
#include <stdlib.h>

#include "app/config_paths.h"
#include "base/config_file.h"
#include "base/file_io.h"
#include "base/sysinfo.h"
#include "json/json.h"
#include "net/http.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/wait.h>
#endif

namespace {

const u64 UPDATE_CHECK_INTERVAL_MS = 24ull * 60 * 60 * 1000;
const u32 UPDATE_CHECK_TIMEOUT_SECONDS = 10;

#if defined(_WIN32)
const char* INSTALL_COMMAND =
    "powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
    "\"[Net.ServicePointManager]::SecurityProtocol = 'Tls12'; "
    "irm " CLANKER_INSTALL_SCRIPT_BASE_URL "install.ps1 | iex\"";
#else
// Download before running: in a plain `curl | sh` pipeline a failed download
// still exits 0, because sh succeeds on empty input.
const char* INSTALL_COMMAND =
    "script=$(curl -fsSL " CLANKER_INSTALL_SCRIPT_BASE_URL "install.sh) && "
    "printf '%s\\n' \"$script\" | sh";
#endif

bool IsSeparator(char c) { return c == '/' || c == '\\'; }

// `value` is UTF-8; the narrow CRT call would read it in the ANSI code page.
void SetEnv(const char* name, const char* value) {
#if defined(_WIN32)
  wchar_t wideName[64];
  wchar_t wideValue[4096];
  wideValue[0] = 0;
  if (!MultiByteToWideChar(CP_UTF8, 0, name, -1, wideName, 64)) return;
  if (value && !MultiByteToWideChar(CP_UTF8, 0, value, -1, wideValue, 4096)) return;
  _wputenv_s(wideName, wideValue);
#else
  if (value)
    setenv(name, value, 1);
  else
    unsetenv(name);
#endif
}

bool FileExists(Arena* arena, StrView path) {
  FILE* f = nullptr;
  if (File_Open(arena, path, "rb", &f) != FileIoResult::Success) return false;
  fclose(f);
  return true;
}

void RunUpdateCheck(void* arg) {
  UpdateCheck* check = (UpdateCheck*)arg;
  Arena mem = ArenaCreate((usize)8 << 20);
  HttpHeader headers[] = {{SVL("accept"), SVL("application/vnd.github+json")}};
  HttpRequest request = {};
  request.Method = SVL("GET");
  request.Url = SVL(CLANKER_RELEASES_API_URL);
  request.Headers = headers;
  request.HeaderCount = 1;
  request.TimeoutSeconds = UPDATE_CHECK_TIMEOUT_SECONDS;
  request.Isolated = true;
  HttpResponse response = Http_Fetch(&mem, &request, &check->Abort);
  // An interrupted check is retried on the next start rather than recorded.
  if (!Atomic_LoadI32(&check->Abort)) {
    UpdateCache cache = {NowMillis(), check->Cached.Latest};
    if (response.Status >= 200 && response.Status < 300) {
      StrView latest = Releases_ParseLatest(&mem, response.Body);
      if (latest.Length) cache.Latest = latest;
    }
    ConfigFile_WriteAtomic(&mem, check->CachePath, UpdateCache_Format(&mem, &cache));
  }
  ArenaRelease(&mem);
}

}  // namespace

bool Version_Parse(StrView text, u32 out[3]) {
  u32 i = 0;
  if (i < text.Length && text.Data[i] == 'v') i++;
  for (u32 part = 0; part < 3; part++) {
    if (part && (i >= text.Length || text.Data[i++] != '.')) return false;
    u32 digits = 0;
    u32 value = 0;
    while (i < text.Length && text.Data[i] >= '0' && text.Data[i] <= '9') {
      if (++digits > 9) return false;
      value = value * 10 + (u32)(text.Data[i++] - '0');
    }
    if (!digits) return false;
    out[part] = value;
  }
  return i == text.Length;
}

i32 Version_Compare(StrView a, StrView b) {
  u32 va[3];
  u32 vb[3];
  if (!Version_Parse(a, va) || !Version_Parse(b, vb)) return 0;
  for (u32 i = 0; i < 3; i++) {
    if (va[i] != vb[i]) return va[i] < vb[i] ? -1 : 1;
  }
  return 0;
}

StrView Releases_ParseLatest(Arena* arena, StrView json) {
  JsonValue* root = Json_ParseCopy(arena, json);
  if (!Json_IsArray(root) || !root->Count) return StrView{};
  StrView tag = Json_GetStr(root->Items[0], "tag_name");
  u32 version[3];
  if (!Version_Parse(tag, version)) return StrView{};
  if (tag.Data[0] == 'v') tag = StrView{tag.Data + 1, tag.Length - 1};
  return StrView_Dup(arena, tag);
}

StrView Install_PrefixFromExe(Arena* arena, StrView exePath) {
  i32 last = -1;
  i32 previous = -1;
  for (u32 i = 0; i < exePath.Length; i++) {
    if (!IsSeparator(exePath.Data[i])) continue;
    previous = last;
    last = (i32)i;
  }
  if (previous < 0) return StrView{};
  StrView bin = {exePath.Data + previous + 1, (u32)(last - previous - 1)};
  if (!StrView_EqInsensitiveAscii(bin, SVL("bin"))) return StrView{};
  StrView prefix = {exePath.Data, (u32)previous};
  StrBuf marker = StrBuf_Make(arena, prefix.Length + 40);
  StrBuf_Append(&marker, prefix);
  StrBuf_AppendCStr(&marker, "/share/clanker/DISTRIBUTION.txt");
  if (!FileExists(arena, StrBuf_View(&marker))) return StrView{};
  return StrView_Dup(arena, prefix);
}

StrView Install_Prefix(Arena* arena) {
  StrView exe = Sys_ExecutablePath(arena);
  return exe.Length ? Install_PrefixFromExe(arena, exe) : StrView{};
}

int RunUpgrade(Arena* arena, const char* version) {
  u32 parsed[3];
  if (version && !Version_Parse(SV(version), parsed)) {
    fprintf(stderr, "error: --version expects X.Y.Z, got '%s'\n", version);
    return 1;
  }
  StrView prefix = Install_Prefix(arena);
  if (!prefix.Length) {
    fprintf(stderr,
            "error: this copy of Clanker was not installed by the install script, "
            "so it cannot upgrade itself.\nInstall it with:\n  %s\n",
            INSTALL_COMMAND);
    return 1;
  }
  SetEnv("CLANKER_INSTALL_DIR", StrView_DupCStr(arena, prefix));
  SetEnv("CLANKER_INSTALL_VERSION", version);
#if defined(_WIN32)
  // Windows PowerShell cannot load PowerShell 7's modules, which it would
  // inherit when `clanker upgrade` runs from a pwsh terminal.
  SetEnv("PSModulePath", nullptr);
#endif
  fflush(stdout);
  fflush(stderr);
  int status = system(INSTALL_COMMAND);
#if defined(_WIN32)
  return status == 0 ? 0 : 1;
#else
  return status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
#endif
}

bool UpdateCache_Parse(Arena* arena, StrView json, UpdateCache* out) {
  *out = {};
  JsonValue* root = Json_ParseCopy(arena, json);
  if (!Json_IsObject(root)) return false;
  i64 checkedAt = Json_GetInt(root, "checked_at", 0);
  out->CheckedAtMs = checkedAt > 0 ? (u64)checkedAt : 0;
  StrView latest = Json_GetStr(root, "latest");
  u32 version[3];
  if (Version_Parse(latest, version)) out->Latest = latest;
  return true;
}

StrView UpdateCache_Format(Arena* arena, const UpdateCache* cache) {
  StrBuf out = StrBuf_Make(arena, 64);
  JsonWriter w;
  JsonWrite_Begin(&w, &out);
  JsonWrite_U64(&w, "checked_at", cache->CheckedAtMs);
  JsonWrite_String(&w, "latest", cache->Latest);
  JsonWrite_End(&w);
  return StrBuf_View(&out);
}

bool UpdateCache_IsStale(const UpdateCache* cache, u64 nowMs) {
  // A timestamp from the future (clock moved back) would otherwise suppress
  // checks until the clock caught up.
  return !cache->CheckedAtMs || nowMs < cache->CheckedAtMs ||
         nowMs - cache->CheckedAtMs >= UPDATE_CHECK_INTERVAL_MS;
}

StrView UpdateNotice(Arena* arena, const UpdateCache* cache, StrView current) {
  if (!cache->Latest.Length || Version_Compare(cache->Latest, current) <= 0) return StrView{};
  StrBuf notice = StrBuf_Make(arena, cache->Latest.Length + current.Length + 80);
  StrBuf_AppendCStr(&notice, "Clanker ");
  StrBuf_Append(&notice, cache->Latest);
  StrBuf_AppendCStr(&notice, " is available (you have ");
  StrBuf_Append(&notice, current);
  StrBuf_AppendCStr(&notice, "). Run `clanker upgrade` to install it.");
  return StrBuf_View(&notice);
}

void UpdateCheck_Start(UpdateCheck* check, Arena* arena) {
  const char* disabled = getenv("CLANKER_NO_UPDATE_CHECK");
  if ((disabled && *disabled) || !Install_Prefix(arena).Length) return;
  check->CachePath = ClankerDataPath(arena, "update-check.json");
  StrView content = {};
  if (ConfigFile_ReadAll(arena, check->CachePath, &content) == ConfigFileReadResult::Success)
    UpdateCache_Parse(arena, content, &check->Cached);
  check->Notice = UpdateNotice(arena, &check->Cached, SVL(CLANKER_VERSION));
  if (!UpdateCache_IsStale(&check->Cached, NowMillis())) return;
  Atomic_StoreI32(&check->Abort, 0);
  check->Running = Thread_Start(&check->Worker, RunUpdateCheck, check);
}

void UpdateCheck_Stop(UpdateCheck* check) {
  if (!check->Running) return;
  Atomic_StoreI32(&check->Abort, 1);
  Thread_Join(&check->Worker);
  check->Running = false;
}
