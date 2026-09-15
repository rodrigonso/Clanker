#pragma once
#if defined(_WIN32)
#include "base/str.h"
#include <string.h>
#include <windows.h>

inline bool Win32_Path(StrView path, wchar_t* out, u32 capacity) {
  if (!path.Data || !path.Length || path.Length > 0x7ffffffeu ||
      capacity < 2 || capacity > 0x7fffffffu || memchr(path.Data, 0, path.Length)) {
    SetLastError(ERROR_INVALID_NAME);
    return false;
  }
  int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.Data,
                                    (int)path.Length, out, (int)capacity - 1);
  if (!length) return false;
  out[length] = 0;
  if (length >= 4 && out[0] == L'\\' && out[1] == L'\\' &&
      (out[2] == L'?' || out[2] == L'.') && out[3] == L'\\') return true;
  bool absolute = (length >= 3 && out[1] == L':' &&
                   (out[2] == L'\\' || out[2] == L'/')) ||
                  (length >= 2 && out[0] == L'\\' && out[1] == L'\\');
  DWORD fullLength = absolute ? (DWORD)length : GetFullPathNameW(out, 0, nullptr, nullptr);
  if (!fullLength) return false;
  // Extended paths avoid MAX_PATH without requiring a process manifest.
  if (fullLength >= 248) {
    wchar_t full[4096];
    fullLength = GetFullPathNameW(out, 4096, full, nullptr);
    if (!fullLength) return false;
    if (fullLength >= 4096) {
      SetLastError(ERROR_FILENAME_EXCED_RANGE);
      return false;
    }
    bool unc = full[0] == L'\\' && full[1] == L'\\';
    u32 skip = unc ? 2 : 0;
    u32 prefix = unc ? 8 : 4;
    if (fullLength - skip + prefix >= capacity) {
      SetLastError(ERROR_FILENAME_EXCED_RANGE);
      return false;
    }
    memcpy(out, unc ? L"\\\\?\\UNC\\" : L"\\\\?\\", prefix * sizeof(wchar_t));
    memcpy(out + prefix, full + skip, (fullLength - skip + 1) * sizeof(wchar_t));
  }
  return true;
}

inline bool Win32_Utf8(Arena* arena, const wchar_t* text, u32 length, StrView* out) {
  *out = {};
  if (!length || length > 0x7fffffffu) {
    SetLastError(ERROR_INVALID_NAME);
    return false;
  }
  int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, (int)length,
                                   nullptr, 0, nullptr, nullptr);
  if (!bytes) return false;
  char* utf8 = (char*)ArenaAlloc(arena, (usize)bytes + 1, 1);
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, (int)length,
                          utf8, bytes, nullptr, nullptr) != bytes) return false;
  utf8[bytes] = 0;
  *out = StrView{utf8, (u32)bytes};
  return true;
}
#endif
