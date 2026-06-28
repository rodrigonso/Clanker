#pragma once
#include "base/str.h"

struct FileLockToken {
  void* Handle;
};

// Optional one-time init (safe to call multiple times). Tools also init lazily.
void FileLock_Init();

// Block until the calling thread has exclusive access to `path` (compare by exact
// string). Returns a token to release. `path` content is copied internally.
FileLockToken FileLock_Acquire(StrView path);

// Release a previously-acquired lock.
void FileLock_Release(FileLockToken tok);
