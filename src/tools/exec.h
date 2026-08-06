#pragma once
#include "base/arena.h"
#include "base/str.h"

// Cross-platform command execution capturing combined stdout+stderr.
struct ExecResult {
  i32 ExitCode;
  bool TimedOut;
  bool Spawned;  // false if the process could not be started
  StrView Output;
};

typedef bool (*ExecChunkFn)(void* user, StrView chunk);

// Run `command` through the platform shell (`cmd /c` on Windows, `/bin/sh -c`
// on POSIX). Aborts the whole process tree if `abort` becomes non-zero or
// `timeoutMs` elapses.
ExecResult Exec_RunStreaming(Arena* arena, StrView command, u32 timeoutMs,
                             volatile i32* abort, ExecChunkFn onData,
                             void* user, StrBuf* output = nullptr);

struct ExecArgvSink {
  void* User;
  ExecChunkFn OnStdout;
  ExecChunkFn OnStderr;
};

struct ExecArgvResult {
  i32 ExitCode;
  bool TimedOut;
  bool Spawned;
};

// Run a program directly without a shell. Arguments are passed verbatim,
// stdout and stderr stay separate, and either sink may stop the process tree.
ExecArgvResult Exec_RunArgv(Arena* arena, StrView program, const StrView* args, u32 argCount,
                            u32 timeoutMs, volatile i32* abort, ExecArgvSink sink);
