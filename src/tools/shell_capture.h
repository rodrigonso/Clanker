#pragma once
#include "base/arena.h"
#include "base/str.h"

// Close consumes the handle even on failure. A failed write/close leaves the
// artifact in place; callers must report its path as incomplete.
struct ShellCaptureIo {
  void* User;
  void* (*Open)(void* user, Arena* arena, StrView directory, StrView* path);
  bool (*Write)(void* user, void* file, StrView bytes);
  bool (*Close)(void* user, void* file);
};

struct ShellCapture {
  Arena* Mem;
  ShellCaptureIo Io;
  StrView Directory;
  char* Tail;
  u32 Head;
  u32 Count;
  u64 TotalBytes;
  u64 Newlines;
  char LastByte;
  void* File;
  StrView Path;
  StrView Error;
  bool Finished;
};

// Null io selects the native artifact writer. All retained buffers and paths
// belong to arena; Feed never allocates in proportion to accumulated output.
void ShellCapture_Init(ShellCapture* capture, Arena* arena, StrView directory,
                       const ShellCaptureIo* io = nullptr);
bool ShellCapture_Feed(ShellCapture* capture, StrView chunk);
bool ShellCapture_Finish(ShellCapture* capture);
u64 ShellCapture_LineCount(const ShellCapture* capture);
StrView ShellCapture_Tail(Arena* scratch, const ShellCapture* capture,
                         bool* byBytes = nullptr);
