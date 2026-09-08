#pragma once
#include "base/str.h"

enum class FileIoResult : u8 {
  Success,
  Missing,
  OpenFailed,
  IoError,
};

// Binary whole-file I/O, including close errors. Successful reads are
// null-terminated at [Length]; failed reads leave `out` empty.
FileIoResult File_ReadAll(Arena* arena, StrView path, StrView* out);

// Truncates in place. Configuration files must use ConfigFile_WriteAtomic.
FileIoResult File_WriteAll(Arena* arena, StrView path, StrView content);
