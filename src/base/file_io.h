#pragma once
#include "base/str.h"
#include <stdio.h>

enum class FileIoResult : u8 {
  Success,
  Missing,
  OpenFailed,
  IoError,
};

// Opens a UTF-8 path in the requested C-runtime mode. The caller owns the FILE.
FileIoResult File_Open(Arena* arena, StrView path, const char* mode, FILE** out);

// Binary whole-file I/O, including close errors. Successful reads are
// null-terminated at [Length]; failed reads leave `out` empty.
FileIoResult File_ReadAll(Arena* arena, StrView path, StrView* out);

// Truncates in place. Configuration files must use ConfigFile_WriteAtomic.
FileIoResult File_WriteAll(Arena* arena, StrView path, StrView content);
