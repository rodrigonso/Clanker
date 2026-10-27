#include "app/browser.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#else
#include <spawn.h>
#include <sys/wait.h>

extern char** environ;
#endif

bool Browser_IsRemoteSession() {
  const char* vars[] = {"SSH_CONNECTION", "SSH_CLIENT", "SSH_TTY"};
  for (const char* name : vars) {
    const char* value = getenv(name);
    if (value && *value) return true;
  }
  return false;
}

bool Browser_Open(StrView url) {
  if (!url.Length || url.Length > 8192 || memchr(url.Data, 0, url.Length)) return false;
#if defined(_WIN32)
  wchar_t wide[8193];
  int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url.Data,
                                   (int)url.Length, wide, 8192);
  if (!length) return false;
  wide[length] = 0;
  HINSTANCE result = ShellExecuteW(nullptr, L"open", wide, nullptr, nullptr, SW_SHOWNORMAL);
  return (INT_PTR)result > 32;
#else
  char buf[8193];
  memcpy(buf, url.Data, url.Length);
  buf[url.Length] = '\0';
  char* argv[] = {(char*)"open", buf, nullptr};
  pid_t pid = 0;
  if (posix_spawnp(&pid, "open", nullptr, nullptr, argv, environ) != 0) return false;
  int status = 0;
  if (waitpid(pid, &status, 0) != pid) return false;
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}
