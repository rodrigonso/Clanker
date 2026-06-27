#include "net/browser.h"

#include <string.h>

#if defined(_WIN32)
#include <windows.h>

#include <shellapi.h>

void OpenBrowser(StrView url) {
  int wlen = MultiByteToWideChar(CP_UTF8, 0, url.Data, (int)url.Length, nullptr, 0);
  if (wlen <= 0 || wlen >= 2048) return;  // auth URLs are ~500 chars
  wchar_t w[2048];
  MultiByteToWideChar(CP_UTF8, 0, url.Data, (int)url.Length, w, wlen);
  w[wlen] = 0;
  ShellExecuteW(nullptr, L"open", w, nullptr, nullptr, SW_SHOWNORMAL);
}

#else
#include <sys/types.h>
#include <unistd.h>

void OpenBrowser(StrView url) {
  char buf[2048];
  if (url.Length >= sizeof(buf)) return;
  memcpy(buf, url.Data, url.Length);
  buf[url.Length] = '\0';

  pid_t pid = fork();
  if (pid == 0) {
#if defined(__APPLE__)
    execlp("open", "open", buf, (char*)nullptr);
#else
    execlp("xdg-open", "xdg-open", buf, (char*)nullptr);
#endif
    _exit(127);  // exec failed
  }
  // Parent: fire-and-forget. The short-lived login process exits soon after.
}

#endif
