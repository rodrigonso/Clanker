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
  HANDLE handle =
      CreateFileA(StrBuf_CStr(&native), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    DWORD error = GetLastError();
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION)
      return ProcessFileLockStatus::Busy;
    return ProcessFileLockStatus::IoError;
  }
  memcpy(out->Storage, &handle, sizeof(handle));
  out->Held = true;
  return ProcessFileLockStatus::Acquired;
}

void ProcessFileLock_Release(ProcessFileLock* lock) {
  if (!lock || !lock->Held) return;
  HANDLE handle = nullptr;
  memcpy(&handle, lock->Storage, sizeof(handle));
  CloseHandle(handle);
  *lock = ProcessFileLock{};
}

#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

static_assert(sizeof(int) <= sizeof(((ProcessFileLock*)nullptr)->Storage),
              "ProcessFileLock storage too small");

ProcessFileLockStatus ProcessFileLock_TryAcquire(Arena* scratch, StrView path,
                                                 ProcessFileLock* out) {
  if (!scratch || !out || path.Length == 0) return ProcessFileLockStatus::IoError;
  *out = ProcessFileLock{};
  StrBuf native = StrBuf_Make(scratch, path.Length + 1);
  StrBuf_Append(&native, path);
  int fd = open(StrBuf_CStr(&native), O_CREAT | O_RDWR, 0600);
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

  memcpy(out->Storage, &fd, sizeof(fd));
  out->Held = true;
  return ProcessFileLockStatus::Acquired;
}

void ProcessFileLock_Release(ProcessFileLock* lock) {
  if (!lock || !lock->Held) return;
  int fd = -1;
  memcpy(&fd, lock->Storage, sizeof(fd));
  close(fd);
  *lock = ProcessFileLock{};
}
#endif
