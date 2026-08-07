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

// Reports whether a live holder owns the lock at `path`, without creating,
// modifying, or removing the sentinel. Answering "is this busy?" by acquiring
// and releasing would author a sentinel per probe -- a listing would write one
// file per session it displays -- and would also own the lock outright for the
// duration. A missing sentinel, or a stale one left by a holder that died
// without unlinking, reads as free. A lock held by the calling process does
// not read as held on POSIX, since F_GETLK reports only conflicting owners;
// callers that care must track their own claims. On Windows the probe still
// holds a read handle for the length of the call, so an acquirer racing it on
// a stale sentinel can briefly observe Busy.
bool ProcessFileLock_IsHeld(Arena* scratch, StrView path);
