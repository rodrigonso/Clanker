#include "base/config_file.h"

#include <errno.h>
#include <stdio.h>

#include "base/file_io.h"

#if defined(_WIN32)
#include <direct.h>
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

  const char* dir = StrView_DupCStr(a, StrView{path.Data, (u32)slash});
  errno = 0;
#if defined(_WIN32)
  int result = _mkdir(dir);
#else
  int result = mkdir(dir, 0700);
#endif
  return result == 0 || errno == EEXIST;
}

bool ConfigFile_WriteAtomic(Arena* a, StrView path, StrView content) {
  if (!ConfigFile_EnsureParentDir(a, path)) return false;

  const char* destination = StrView_DupCStr(a, path);
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

  FILE* f = fopen(StrBuf_CStr(&temporary), "wb");
  if (!f) return false;

#if !defined(_WIN32)
  if (chmod(StrBuf_CStr(&temporary), 0600) != 0) {
    fclose(f);
    remove(StrBuf_CStr(&temporary));
    return false;
  }
#endif

  bool ok = content.Length == 0 || fwrite(content.Data, 1, content.Length, f) == content.Length;
  if (ok && fflush(f) != 0) ok = false;
  if (fclose(f) != 0) ok = false;
  if (!ok) {
    remove(StrBuf_CStr(&temporary));
    return false;
  }

#if defined(_WIN32)
  if (!MoveFileExA(StrBuf_CStr(&temporary), destination,
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    remove(StrBuf_CStr(&temporary));
    return false;
  }
#else
  if (rename(StrBuf_CStr(&temporary), destination) != 0) {
    remove(StrBuf_CStr(&temporary));
    return false;
  }
#endif
  return true;
}
