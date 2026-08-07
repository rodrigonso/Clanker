#include "base/process_file_lock.h"

#include <string.h>

#if defined(_WIN32)
#include <windows.h>

static_assert(sizeof(HANDLE) <= sizeof(((ProcessFileLock*)nullptr)->Storage),
              "ProcessFileLock storage too small");

ProcessFileLockStatus ProcessFileLock_TryAcquire(Arena* scratch, StrView path,
                                                 ProcessFileLock* out) {
  if (!scratch || !out || path.Length == 0) return ProcessFileLockStatus::IoError;
  *out = ProcessFileLock{};
  StrBuf native = StrBuf_Make(scratch, path.Length + 1);
  StrBuf_Append(&native, path);
  // DELETE access plus FILE_FLAG_DELETE_ON_CLOSE ties the sentinel file's
  // lifetime to the handle, so the kernel removes it even if this process is
  // killed. A leftover file from an older build is opened and then cleaned up
  // the same way.
  HANDLE handle = CreateFileA(StrBuf_CStr(&native),
                              GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr,
                              OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE,
                              nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    DWORD error = GetLastError();
    // While a holder's delete is still pending the name cannot be reopened.
    // That is the same answer as a sharing violation -- someone else owns the
    // lock right now -- so it must not surface as an I/O failure.
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION ||
        error == ERROR_ACCESS_DENIED || error == ERROR_DELETE_PENDING)
      return ProcessFileLockStatus::Busy;
    return ProcessFileLockStatus::IoError;
  }
  memcpy(out->Storage, &handle, sizeof(handle));
  out->Held = true;
  return ProcessFileLockStatus::Acquired;
}

void ProcessFileLock_Release(ProcessFileLock* lock, StrView path) {
  (void)path;  // The open handle carries the deletion; see TryAcquire.
  if (!lock || !lock->Held) return;
  HANDLE handle = nullptr;
  memcpy(&handle, lock->Storage, sizeof(handle));
  CloseHandle(handle);
  *lock = ProcessFileLock{};
}

#else
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static_assert(sizeof(int) <= sizeof(((ProcessFileLock*)nullptr)->Storage),
              "ProcessFileLock storage too small");

namespace {

// Copies `path` into a null-terminated stack buffer for the POSIX calls that
// need one. Returns false when the path does not fit, in which case callers
// skip the operation rather than truncate and act on the wrong file.
bool NativePath(StrView path, char* buffer, u32 capacity) {
  if (path.Length == 0 || path.Length + 1 > capacity) return false;
  memcpy(buffer, path.Data, path.Length);
  buffer[path.Length] = '\0';
  return true;
}

}  // namespace

ProcessFileLockStatus ProcessFileLock_TryAcquire(Arena* scratch, StrView path,
                                                 ProcessFileLock* out) {
  if (!scratch || !out || path.Length == 0) return ProcessFileLockStatus::IoError;
  *out = ProcessFileLock{};
  StrBuf native = StrBuf_Make(scratch, path.Length + 1);
  StrBuf_Append(&native, path);
  const char* target = StrBuf_CStr(&native);

  // A releasing holder unlinks the sentinel while it still holds the lock, so
  // an open() that raced with it can land on an inode that is no longer the
  // one published at `path`. Locking that dead inode would let a third process
  // create a fresh file and hold the same logical lock concurrently, so the
  // inode is rechecked after locking and a losing attempt is retried.
  for (u32 attempt = 0; attempt < 8; attempt++) {
    int fd = open(target, O_CREAT | O_RDWR, 0600);
    if (fd < 0) return ProcessFileLockStatus::IoError;

    struct flock claim = {};
    claim.l_type = F_WRLCK;
    claim.l_whence = SEEK_SET;
    claim.l_start = 0;
    claim.l_len = 0;
    if (fcntl(fd, F_SETLK, &claim) != 0) {
      int error = errno;
      close(fd);
      if (error == EACCES || error == EAGAIN) return ProcessFileLockStatus::Busy;
      return ProcessFileLockStatus::IoError;
    }

    struct stat locked = {};
    struct stat published = {};
    if (fstat(fd, &locked) != 0) {
      close(fd);
      return ProcessFileLockStatus::IoError;
    }
    if (stat(target, &published) == 0) {
      if (locked.st_dev == published.st_dev && locked.st_ino == published.st_ino) {
        memcpy(out->Storage, &fd, sizeof(fd));
        out->Held = true;
        return ProcessFileLockStatus::Acquired;
      }
    } else if (errno != ENOENT) {
      close(fd);
      return ProcessFileLockStatus::IoError;
    }
    close(fd);
  }
  // Persistently losing the race means another process is actively cycling the
  // lock, which is the same practical answer as finding it held.
  return ProcessFileLockStatus::Busy;
}

void ProcessFileLock_Release(ProcessFileLock* lock, StrView path) {
  if (!lock || !lock->Held) return;
  int fd = -1;
  memcpy(&fd, lock->Storage, sizeof(fd));
  // Unlink before dropping the lock. A racing acquirer then either fails
  // F_SETLK (this process still holds it) or notices the inode it locked is no
  // longer published at `path` and retries.
  char native[4096];
  if (NativePath(path, native, (u32)sizeof(native))) unlink(native);
  close(fd);
  *lock = ProcessFileLock{};
}
#endif
