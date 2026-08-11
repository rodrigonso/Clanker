#pragma once
#include "base/arena.h"
#include "base/str.h"

// Bound on a stdin payload handed to a child process. The write happens before
// the wait loop, so anything larger than the pipe buffer would block past both
// the timeout and `abort` against a child that never reads its stdin. Windows
// sizes its pipe to exactly this; POSIX pipe buffers cannot be sized portably,
// so the value stays under the smallest common default (16 KiB on macOS).
constexpr u32 EXEC_INPUT_PIPE_BYTES = 1u << 14;

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

// Run `command` through the platform shell with `input` written to the child's
// stdin, which is then closed. Unlike the calls above the child never inherits
// the caller's stdin -- the TUI holds the console in raw mode. `input` must be
// under 16 KiB, which is enforced on both platforms.
//
// When `output` is null the child's stdout goes to the null device. When it is
// non-null, stdout is captured into it and drained throughout the wait loop, so
// a child that outproduces the pipe buffer cannot wedge. stderr always goes to
// the null device either way, which keeps a decision hook's stdout a clean
// channel that diagnostics cannot corrupt.
//
// `outputLimit` bounds what is kept, in bytes; zero means unbounded. Draining
// continues past it, so a runaway child still finishes and is still killed by
// `timeoutMs` -- only the memory is capped. A caller that must tell a complete
// answer from a clipped one sizes the limit above the largest answer it accepts
// and treats hitting it as overflow.
ExecResult Exec_RunShellInput(Arena* arena, StrView command, StrView input,
                              u32 timeoutMs, volatile i32* abort,
                              StrBuf* output = nullptr, u32 outputLimit = 0);
