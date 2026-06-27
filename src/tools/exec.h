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

// Run `command` through the platform shell (`cmd /c` on Windows, `/bin/sh -c`
// on POSIX). Aborts if `abort` becomes non-zero or `timeoutMs` elapses.
ExecResult Exec_Run(Arena* arena, StrView command, u32 timeoutMs, volatile i32* abort);
