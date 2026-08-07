#pragma once

#include "base/arena.h"
#include "base/basics.h"
#include "base/str.h"

enum class ProcessFileLockStatus : u8 { Acquired, Busy, IoError };

struct ProcessFileLock {
  alignas(8) byte Storage[16];
  bool Held;
};

// Ownership is tied to the open OS handle/file descriptor, so process death
// releases the lock without stale-lock detection. The sentinel file is removed
// on release -- including on crash under Windows, where the kernel deletes it
// with the handle -- so lock files do not accumulate beside session data.
ProcessFileLockStatus ProcessFileLock_TryAcquire(Arena* scratch, StrView path,
                                                 ProcessFileLock* out);
// `path` must be the path the lock was acquired with, since that is what gets
// unlinked. An empty path releases the lock but leaves the file behind.
void ProcessFileLock_Release(ProcessFileLock* lock, StrView path);
