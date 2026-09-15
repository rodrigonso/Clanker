#include "base/file_io.h"
#include "base/win32_utf8.h"

#include <errno.h>
#include <stdio.h>

FileIoResult File_Open(Arena* arena, StrView path, const char* mode, FILE** out) {
  *out = nullptr;
  errno = 0;
#if defined(_WIN32)
  (void)arena;
  wchar_t native[4096];
  wchar_t nativeMode[8] = {};
  StrView modeView = SV(mode);
  if (!modeView.Length || modeView.Length >= 8 || !Win32_Path(path, native, 4096)) {
    errno = EINVAL;
    return FileIoResult::OpenFailed;
  }
  for (u32 i = 0; i < modeView.Length; i++) nativeMode[i] = (wchar_t)(u8)mode[i];
  *out = _wfopen(native, nativeMode);
  int error = errno;
#else
  usize mark = arena->Used;
  *out = fopen(StrView_DupCStr(arena, path), mode);
  int error = errno;
  ArenaRestore(arena, mark);
#endif
  if (*out) return FileIoResult::Success;
  return error == ENOENT ? FileIoResult::Missing : FileIoResult::OpenFailed;
}

FileIoResult File_ReadAll(Arena* arena, StrView path, StrView* out) {
  *out = {};
  FILE* file = nullptr;
  FileIoResult opened = File_Open(arena, path, "rb", &file);
  if (opened != FileIoResult::Success) return opened;

  usize mark = arena->Used;
  char chunk[4096];
  StrBuf content = StrBuf_Make(arena, (u32)sizeof(chunk) + 1);
  size_t count = 0;
  while ((count = fread(chunk, 1, sizeof(chunk), file)) > 0)
    StrBuf_Append(&content, StrView{chunk, (u32)count});
  bool ok = ferror(file) == 0;
  if (fclose(file) != 0) ok = false;
  if (!ok) {
    ArenaRestore(arena, mark);
    return FileIoResult::IoError;
  }
  *out = StrBuf_View(&content);
  return FileIoResult::Success;
}

FileIoResult File_WriteAll(Arena* arena, StrView path, StrView content) {
  FILE* file = nullptr;
  FileIoResult opened = File_Open(arena, path, "wb", &file);
  if (opened != FileIoResult::Success) return opened;
  bool ok = !content.Length ||
            fwrite(content.Data, 1, content.Length, file) == content.Length;
  if (fclose(file) != 0) ok = false;
  return ok ? FileIoResult::Success : FileIoResult::IoError;
}
