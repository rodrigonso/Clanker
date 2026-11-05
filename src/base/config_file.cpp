#include "base/config_file.h"

#include <stdio.h>

#include "base/file_io.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

static u32 g_configTempCounter;

ConfigFileReadResult ConfigFile_ReadAll(Arena* a, StrView path, StrView* out) {
  FileIoResult result = File_ReadAll(a, path, out);
  if (result == FileIoResult::Success) return ConfigFileReadResult::Success;
  if (result == FileIoResult::Missing) return ConfigFileReadResult::Missing;
  return ConfigFileReadResult::Failure;
}

bool ConfigFile_EnsureParentDir(Arena* a, StrView path) {
  i32 slash = -1;
  for (u32 i = 0; i < path.Length; i++) {
    if (path.Data[i] == '/' || path.Data[i] == '\\') slash = (i32)i;
  }
  if (slash <= 0) return true;
  return File_MakeDir(a, StrView{path.Data, (u32)slash});
}

bool ConfigFile_WriteAtomic(Arena* a, StrView path, StrView content) {
  if (!ConfigFile_EnsureParentDir(a, path)) return false;

  StrBuf temporary = StrBuf_Make(a, path.Length + 48);
  StrBuf_Append(&temporary, path);
  StrBuf_AppendCStr(&temporary, ".tmp-");
#if defined(_WIN32)
  StrBuf_AppendU64(&temporary, (u64)GetCurrentProcessId());
#else
  StrBuf_AppendU64(&temporary, (u64)(u32)getpid());
#endif
  StrBuf_AppendChar(&temporary, '-');
  StrBuf_AppendU64(&temporary, ++g_configTempCounter);
  StrView temporaryPath = StrBuf_View(&temporary);

  FILE* f = nullptr;
  if (File_Open(a, temporaryPath, "wb", &f) != FileIoResult::Success) return false;

#if !defined(_WIN32)
  if (fchmod(fileno(f), 0600) != 0) {
    fclose(f);
    File_Remove(a, temporaryPath);
    return false;
  }
#endif

  bool ok = content.Length == 0 || fwrite(content.Data, 1, content.Length, f) == content.Length;
  if (ok && fflush(f) != 0) ok = false;
  if (fclose(f) != 0) ok = false;
  if (!ok || !File_Replace(a, temporaryPath, path)) {
    File_Remove(a, temporaryPath);
    return false;
  }
  return true;
}
