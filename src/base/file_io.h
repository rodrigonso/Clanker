#pragma once
#include "base/str.h"
#include <stdio.h>

enum class FileIoResult : u8 {
  Success,
  Missing,
  OpenFailed,
  IoError,
  TooLarge,
};

// The most File_ReadAll can return: StrView lengths are u32, and one byte is
// kept for the terminating NUL.
constexpr u64 FILE_READ_MAX_BYTES = 0xFFFFFFFEull;

// Opens a UTF-8 path in the requested C-runtime mode. The caller owns the FILE.
FileIoResult File_Open(Arena* arena, StrView path, const char* mode, FILE** out);

struct FileReader {
  void* Handle;
};

// Binary streaming I/O with caller-owned buffers. A successful zero-byte read
// means EOF. Errors clear `count`; close clears the handle, including on error.
// Closing an empty reader succeeds. Open with an empty reader.
FileIoResult File_OpenRead(Arena* scratch, StrView path, FileReader* out);
FileIoResult File_ReadChunk(FileReader* file, void* buffer, u32 capacity, u32* count);
FileIoResult File_Close(FileReader* file);

// Binary whole-file I/O, including close errors. Successful reads are
// null-terminated at [Length]; failed reads leave `out` empty and give back
// any arena space they used. A file longer than `maxBytes` fails with TooLarge
// before its contents are buffered. Regular files are read into one allocation
// sized from their length, so the arena cost is the file size plus one byte.
FileIoResult File_ReadAll(Arena* arena, StrView path, StrView* out,
                          u64 maxBytes = FILE_READ_MAX_BYTES);

// Truncates in place. Configuration files must use ConfigFile_WriteAtomic.
FileIoResult File_WriteAll(Arena* arena, StrView path, StrView content);

// UTF-8 path counterparts of remove/rename/mkdir. The narrow C-runtime calls
// take the ANSI code page on Windows, which cannot name every path.
// `arena` is only borrowed for a null-terminated copy and is restored.
bool File_Remove(Arena* arena, StrView path);
// Atomically replaces `to` with `from` (rename(2); MoveFileEx with
// REPLACE_EXISTING and WRITE_THROUGH on Windows).
bool File_Replace(Arena* arena, StrView from, StrView to);
// True when the directory was created or already exists. POSIX mode is 0700.
bool File_MakeDir(Arena* arena, StrView path);
