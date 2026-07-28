#pragma once

#include "base/arena.h"
#include "base/basics.h"
#include "base/str.h"

enum class ProcessFileLockStatus : u8 { Acquired, Busy, IoError };

struct ProcessFileLock {
  alignas(8) byte Storage[16];
  bool Held;
};

// The lock file may remain on disk after release. Ownership is tied to the open
// OS handle/file descriptor, so process death releases it without stale-lock
// detection or sentinel deletion.
ProcessFileLockStatus ProcessFileLock_TryAcquire(Arena* scratch, StrView path,
                                                 ProcessFileLock* out);
void ProcessFileLock_Release(ProcessFileLock* lock);
