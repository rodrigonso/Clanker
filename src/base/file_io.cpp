#include "base/file_io.h"
#include "base/win32_utf8.h"

#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32)
#include <direct.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

// The length of a regular file. Anything else (a pipe, a device) has no size
// worth trusting up front and reports false.
bool RegularFileSize(FILE* file, u64* out) {
#if defined(_WIN32)
  struct _stat64 info;
  if (_fstat64(_fileno(file), &info) != 0 || (info.st_mode & _S_IFMT) != _S_IFREG)
    return false;
#else
  struct stat info;
  if (fstat(fileno(file), &info) != 0 || !S_ISREG(info.st_mode)) return false;
#endif
  *out = (u64)info.st_size;
  return true;
}

constexpr u64 FILE_READ_GROWTH_FLOOR = (u64)64 << 10;

}  // namespace

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

FileIoResult File_OpenRead(Arena* scratch, StrView path, FileReader* out) {
  FILE* file = nullptr;
  FileIoResult result = File_Open(scratch, path, "rb", &file);
  out->Handle = file;
  return result;
}

FileIoResult File_ReadChunk(FileReader* file, void* buffer, u32 capacity, u32* count) {
  *count = 0;
  if (!file->Handle || !buffer || !capacity) return FileIoResult::IoError;
  FILE* handle = (FILE*)file->Handle;
  size_t read = fread(buffer, 1, capacity, handle);
  if (ferror(handle)) return FileIoResult::IoError;
  *count = (u32)read;
  return FileIoResult::Success;
}

FileIoResult File_Close(FileReader* file) {
  FILE* handle = (FILE*)file->Handle;
  file->Handle = nullptr;
  return !handle || fclose(handle) == 0 ? FileIoResult::Success : FileIoResult::IoError;
}

FileIoResult File_ReadAll(Arena* arena, StrView path, StrView* out, u64 maxBytes) {
  *out = {};
  if (maxBytes > FILE_READ_MAX_BYTES) maxBytes = FILE_READ_MAX_BYTES;
  FILE* file = nullptr;
  FileIoResult opened = File_Open(arena, path, "rb", &file);
  if (opened != FileIoResult::Success) return opened;

  u64 size = 0;
  bool sized = RegularFileSize(file, &size);
  if (sized && size > maxBytes) {
    fclose(file);
    return FileIoResult::TooLarge;
  }

  usize mark = arena->Used;
  u64 capacity = sized ? size : FILE_READ_GROWTH_FLOOR;
  if (capacity > maxBytes) capacity = maxBytes;
  char* data = (char*)ArenaAlloc(arena, (usize)capacity + 1, 1);
  u64 length = 0;
  FileIoResult result = FileIoResult::Success;
  for (;;) {
    if (length == capacity) {
      // A full buffer is the common end of a regular file. Probing one byte
      // tells that apart from a file that grew since the stat, or a source
      // with no size, without allocating for data that is not there.
      int next = fgetc(file);
      if (next == EOF) break;
      if (capacity >= maxBytes) {
        result = FileIoResult::TooLarge;
        break;
      }
      u64 grown = capacity + (capacity > FILE_READ_GROWTH_FLOOR
                                  ? capacity
                                  : FILE_READ_GROWTH_FLOOR);
      if (grown > maxBytes) grown = maxBytes;
      // Extending in place keeps the read to one block: nothing else
      // allocates from `arena` while this runs, so the next bytes are the
      // ones right after the buffer's terminator slot.
      char* extension = (char*)ArenaAlloc(arena, (usize)(grown - capacity), 1);
      FFI(extension != data + capacity + 1);
      capacity = grown;
      data[length++] = (char)next;
      continue;
    }
    size_t count = fread(data + length, 1, (size_t)(capacity - length), file);
    if (count == 0) break;
    length += count;
  }
  if (result == FileIoResult::Success && ferror(file)) result = FileIoResult::IoError;
  if (fclose(file) != 0 && result == FileIoResult::Success)
    result = FileIoResult::IoError;
  if (result != FileIoResult::Success) {
    ArenaRestore(arena, mark);
    return result;
  }
  data[length] = 0;
  *out = StrView{data, (u32)length};
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

bool File_Remove(Arena* arena, StrView path) {
#if defined(_WIN32)
  (void)arena;
  wchar_t native[4096];
  return Win32_Path(path, native, 4096) && _wremove(native) == 0;
#else
  usize mark = arena->Used;
  bool ok = remove(StrView_DupCStr(arena, path)) == 0;
  ArenaRestore(arena, mark);
  return ok;
#endif
}

bool File_Replace(Arena* arena, StrView from, StrView to) {
#if defined(_WIN32)
  (void)arena;
  wchar_t nativeFrom[4096];
  wchar_t nativeTo[4096];
  return Win32_Path(from, nativeFrom, 4096) && Win32_Path(to, nativeTo, 4096) &&
         MoveFileExW(nativeFrom, nativeTo,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  usize mark = arena->Used;
  bool ok = rename(StrView_DupCStr(arena, from), StrView_DupCStr(arena, to)) == 0;
  ArenaRestore(arena, mark);
  return ok;
#endif
}

bool File_MakeDir(Arena* arena, StrView path) {
  errno = 0;
#if defined(_WIN32)
  (void)arena;
  wchar_t native[4096];
  if (!Win32_Path(path, native, 4096)) return false;
  int result = _wmkdir(native);
#else
  usize mark = arena->Used;
  int result = mkdir(StrView_DupCStr(arena, path), 0700);
  ArenaRestore(arena, mark);
#endif
  return result == 0 || errno == EEXIST;
}

FileIoResult File_Append(Arena* arena, StrView path, StrView content) {
#if defined(_WIN32)
  (void)arena;
  wchar_t native[4096];
  if (!Win32_Path(path, native, 4096)) return FileIoResult::OpenFailed;
  // Append-only access makes the kernel place every write at the current end
  // of file, atomically across processes. The C runtime's append mode seeks
  // and then writes, so two appenders can land on one offset and lose a record.
  HANDLE file = CreateFileW(native, FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return GetLastError() == ERROR_PATH_NOT_FOUND ? FileIoResult::Missing
                                                  : FileIoResult::OpenFailed;
  DWORD written = 0;
  bool ok = !content.Length ||
            (WriteFile(file, content.Data, content.Length, &written, nullptr) &&
             written == content.Length);
  if (!CloseHandle(file)) ok = false;
  return ok ? FileIoResult::Success : FileIoResult::IoError;
#else
  FILE* file = nullptr;
  FileIoResult opened = File_Open(arena, path, "ab", &file);
  if (opened != FileIoResult::Success) return opened;
  // Unbuffered, so the record reaches O_APPEND as a single write.
  setvbuf(file, nullptr, _IONBF, 0);
  bool ok = !content.Length ||
            fwrite(content.Data, 1, content.Length, file) == content.Length;
  if (fclose(file) != 0) ok = false;
  return ok ? FileIoResult::Success : FileIoResult::IoError;
#endif
}
