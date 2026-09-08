#include "base/file_io.h"

#include <errno.h>
#include <stdio.h>

namespace {

FileIoResult OpenFile(Arena* arena, StrView path, const char* mode, FILE** out) {
  usize mark = arena->Used;
  errno = 0;
  *out = fopen(StrView_DupCStr(arena, path), mode);
  int error = errno;
  ArenaRestore(arena, mark);
  if (*out) return FileIoResult::Success;
  return error == ENOENT ? FileIoResult::Missing : FileIoResult::OpenFailed;
}

}  // namespace

FileIoResult File_ReadAll(Arena* arena, StrView path, StrView* out) {
  *out = {};
  FILE* file = nullptr;
  FileIoResult opened = OpenFile(arena, path, "rb", &file);
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
  FileIoResult opened = OpenFile(arena, path, "wb", &file);
  if (opened != FileIoResult::Success) return opened;
  bool ok = !content.Length ||
            fwrite(content.Data, 1, content.Length, file) == content.Length;
  if (fclose(file) != 0) ok = false;
  return ok ? FileIoResult::Success : FileIoResult::IoError;
}
