#include "tools/filelock.h"

#include "base/thread.h"

#include <string.h>

static const u32 FILE_LOCK_MAX_ENTRIES = 256;
static const u32 FILE_LOCK_MAX_PATH = 1024;

struct FileLockEntry {
  char Path[FILE_LOCK_MAX_PATH];
  u32 PathLen;
  Mutex Lock;
  u32 RefCount;
  bool InUse;
};

static FileLockEntry FileLock_Entries[FILE_LOCK_MAX_ENTRIES];
static FileLockEntry FileLock_Fallback;
static bool FileLock_FallbackInit;
static volatile long FileLock_RegistryGuard;

static void FileLock_GuardLock() {
#if defined(_MSC_VER)
  while (_InterlockedCompareExchange(&FileLock_RegistryGuard, 1, 0) != 0) {
    _mm_pause();
  }
#else
  while (!__sync_bool_compare_and_swap(&FileLock_RegistryGuard, 0, 1)) {
#if defined(__i386__) || defined(__x86_64__)
    __builtin_ia32_pause();
#endif
  }
#endif
}

static void FileLock_GuardUnlock() {
#if defined(_MSC_VER)
  _InterlockedExchange(&FileLock_RegistryGuard, 0);
#else
  __atomic_store_n(&FileLock_RegistryGuard, 0, __ATOMIC_RELEASE);
#endif
}

static u32 FileLock_CopyPath(char* dst, StrView path) {
  // Paths at or beyond the fixed buffer size are compared by their truncated
  // first 1024 bytes; this preserves safety but may over-serialize long paths.
  u32 n = path.Length < FILE_LOCK_MAX_PATH ? path.Length : FILE_LOCK_MAX_PATH;
  if (n) memcpy(dst, path.Data, n);
  return n;
}

static bool FileLock_PathEq(const FileLockEntry* e, const char* path, u32 pathLen) {
  return e->PathLen == pathLen && (pathLen == 0 || memcmp(e->Path, path, pathLen) == 0);
}

void FileLock_Init() {
  // Static storage is zero-initialized. Entry and fallback mutexes are created
  // lazily under the atomic registry guard, so explicit init is optional.
}

FileLockToken FileLock_Acquire(StrView path) {
  char key[FILE_LOCK_MAX_PATH];
  u32 keyLen = FileLock_CopyPath(key, path);

  FileLock_GuardLock();

  FileLockEntry* entry = nullptr;
  FileLockEntry* freeEntry = nullptr;
  for (u32 i = 0; i < FILE_LOCK_MAX_ENTRIES; i++) {
    FileLockEntry* it = &FileLock_Entries[i];
    if (it->InUse) {
      if (FileLock_PathEq(it, key, keyLen)) {
        entry = it;
        break;
      }
    } else if (!freeEntry) {
      freeEntry = it;
    }
  }

  if (!entry) {
    if (freeEntry) {
      entry = freeEntry;
      Mutex_Init(&entry->Lock);
      if (keyLen) memcpy(entry->Path, key, keyLen);
      entry->PathLen = keyLen;
      entry->RefCount = 0;
      entry->InUse = true;
    } else {
      // Registry exhaustion is rare; fall back to one shared mutex so
      // correctness is preserved, at the cost of over-serializing overflow paths.
      entry = &FileLock_Fallback;
      if (!FileLock_FallbackInit) {
        Mutex_Init(&entry->Lock);
        FileLock_FallbackInit = true;
      }
    }
  }

  entry->RefCount++;
  FileLock_GuardUnlock();

  Mutex_Lock(&entry->Lock);
  return FileLockToken{entry};
}

void FileLock_Release(FileLockToken tok) {
  if (!tok.Handle) return;

  FileLockEntry* entry = (FileLockEntry*)tok.Handle;
  Mutex_Unlock(&entry->Lock);

  FileLock_GuardLock();
  if (entry->RefCount) entry->RefCount--;
  if (entry != &FileLock_Fallback && entry->RefCount == 0) {
    entry->InUse = false;
    entry->PathLen = 0;
    Mutex_Destroy(&entry->Lock);
  }
  FileLock_GuardUnlock();
}
