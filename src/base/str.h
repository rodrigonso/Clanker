#pragma once
#include <string.h>

#include "base/arena.h"
#include "base/basics.h"

// Non-owning UTF-8 view. The universal "string" passed around the agent.
struct StrView {
  const char* Data;
  u32 Length;
};

// View from a string literal (length known at compile time).
#define SVL(lit) \
  StrView { (lit), (u32)(sizeof(lit) - 1) }

inline StrView SV(const char* s) {
  return StrView{s, s ? (u32)strlen(s) : 0u};
}

inline bool StrView_Eq(StrView a, StrView b) {
  return a.Length == b.Length && (a.Length == 0 || memcmp(a.Data, b.Data, a.Length) == 0);
}

inline bool StrView_EqCStr(StrView a, const char* b) { return StrView_Eq(a, SV(b)); }

inline bool StrView_Contains(StrView hay, const char* needle) {
  u32 nl = (u32)strlen(needle);
  if (nl == 0) return true;
  if (nl > hay.Length) return false;
  for (u32 i = 0; i + nl <= hay.Length; i++) {
    if (memcmp(hay.Data + i, needle, nl) == 0) return true;
  }
  return false;
}

inline bool Ascii_IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
         c == '\v';
}

inline char Ascii_ToLower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

inline bool StrView_EqInsensitiveAscii(StrView a, StrView b) {
  if (a.Length != b.Length) return false;
  for (u32 i = 0; i < a.Length; i++)
    if (Ascii_ToLower(a.Data[i]) != Ascii_ToLower(b.Data[i])) return false;
  return true;
}

inline bool StrView_ContainsInsensitiveAscii(StrView haystack, StrView needle) {
  if (needle.Length == 0) return true;
  if (needle.Length > haystack.Length) return false;
  for (u32 i = 0; i + needle.Length <= haystack.Length; i++) {
    u32 k = 0;
    for (; k < needle.Length; k++) {
      if (Ascii_ToLower(haystack.Data[i + k]) !=
          Ascii_ToLower(needle.Data[k]))
        break;
    }
    if (k == needle.Length) return true;
  }
  return false;
}

inline StrView StrView_Trim(StrView s) {
  u32 first = 0;
  while (first < s.Length && Ascii_IsSpace(s.Data[first])) first++;
  u32 last = s.Length;
  while (last > first && Ascii_IsSpace(s.Data[last - 1])) last--;
  return StrView{s.Data ? s.Data + first : nullptr, last - first};
}

inline bool StrView_StartsWith(StrView value, StrView prefix) {
  return prefix.Length <= value.Length &&
         (prefix.Length == 0 || memcmp(value.Data, prefix.Data, prefix.Length) == 0);
}

inline bool StrView_EndsWithInsensitiveAscii(StrView value, StrView suffix) {
  if (suffix.Length > value.Length) return false;
  u32 offset = value.Length - suffix.Length;
  for (u32 i = 0; i < suffix.Length; i++) {
    if (Ascii_ToLower(value.Data[offset + i]) != Ascii_ToLower(suffix.Data[i]))
      return false;
  }
  return true;
}

// Arena-backed growable UTF-8 builder. Always keeps a null terminator at
// [Length]. Reallocation on growth may move Data, so take a StrView only after
// building is complete.
struct StrBuf {
  Arena* Backing;
  char* Data;
  u32 Length;
  u32 Capacity;  // total allocated bytes, including the null slot
};

inline StrBuf StrBuf_Make(Arena* arena, u32 capacity = 64) {
  if (capacity < 8) capacity = 8;
  StrBuf b;
  b.Backing = arena;
  b.Data = (char*)ArenaAlloc(arena, capacity, 1);
  b.Length = 0;
  b.Capacity = capacity;
  b.Data[0] = '\0';
  return b;
}

inline void StrBuf_Ensure(StrBuf* b, u32 additional) {
  u32 need = b->Length + additional + 1;  // +1 for null terminator
  if (need <= b->Capacity) return;
  u32 newCap = b->Capacity ? b->Capacity * 2 : 8;
  while (newCap < need) newCap *= 2;
  char* nd = (char*)ArenaAlloc(b->Backing, newCap, 1);
  memcpy(nd, b->Data, b->Length);
  b->Data = nd;
  b->Capacity = newCap;
}

inline void StrBuf_Append(StrBuf* b, StrView s) {
  if (s.Length == 0) return;
  StrBuf_Ensure(b, s.Length);
  memcpy(b->Data + b->Length, s.Data, s.Length);
  b->Length += s.Length;
  b->Data[b->Length] = '\0';
}

inline void StrBuf_AppendCStr(StrBuf* b, const char* s) { StrBuf_Append(b, SV(s)); }

inline void StrBuf_AppendChar(StrBuf* b, char c) {
  StrBuf_Ensure(b, 1);
  b->Data[b->Length++] = c;
  b->Data[b->Length] = '\0';
}

inline void StrBuf_AppendU64(StrBuf* b, u64 v) {
  if (v == 0) {
    StrBuf_AppendChar(b, '0');
    return;
  }
  char tmp[20];
  int n = 0;
  while (v) {
    tmp[n++] = (char)('0' + (int)(v % 10));
    v /= 10;
  }
  StrBuf_Ensure(b, (u32)n);
  for (int i = n - 1; i >= 0; --i) b->Data[b->Length++] = tmp[i];
  b->Data[b->Length] = '\0';
}

inline void StrBuf_AppendI64(StrBuf* b, i64 v) {
  if (v < 0) {
    StrBuf_AppendChar(b, '-');
    StrBuf_AppendU64(b, (u64)(~(u64)v + 1));  // negate without UB at INT64_MIN
  } else {
    StrBuf_AppendU64(b, (u64)v);
  }
}

// Append one scalar as UTF-8. Surrogates and out-of-range values have no UTF-8
// form -- encoding them anyway yields CESU-8 that JSON consumers reject as
// "surrogates not allowed" -- so they become U+FFFD instead.
inline void StrBuf_AppendUtf8(StrBuf* b, u32 cp) {
  if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) cp = 0xFFFD;
  if (cp < 0x80) {
    StrBuf_AppendChar(b, (char)cp);
  } else if (cp < 0x800) {
    StrBuf_AppendChar(b, (char)(0xC0 | (cp >> 6)));
    StrBuf_AppendChar(b, (char)(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    StrBuf_AppendChar(b, (char)(0xE0 | (cp >> 12)));
    StrBuf_AppendChar(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
    StrBuf_AppendChar(b, (char)(0x80 | (cp & 0x3F)));
  } else {
    StrBuf_AppendChar(b, (char)(0xF0 | (cp >> 18)));
    StrBuf_AppendChar(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
    StrBuf_AppendChar(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
    StrBuf_AppendChar(b, (char)(0x80 | (cp & 0x3F)));
  }
}

inline StrView StrBuf_View(StrBuf* b) { return StrView{b->Data, b->Length}; }
inline const char* StrBuf_CStr(StrBuf* b) { return b->Data; }

inline void StrBuf_Clear(StrBuf* b) {
  b->Length = 0;
  b->Data[0] = '\0';
}

// Framing for byte streams that arrive in arbitrary chunks: hand every complete
// '\n'-terminated line in `b` to `onLine` (stripping an optional trailing '\r'),
// then compact the trailing partial line to the front so the next chunk can be
// appended. Returns false, leaving the buffer untouched, as soon as `onLine`
// asks to stop.
inline bool StrBuf_DrainLines(StrBuf* b, void* user, bool (*onLine)(void*, StrView)) {
  u32 start = 0;
  for (u32 i = 0; i < b->Length; i++) {
    if (b->Data[i] != '\n') continue;
    u32 length = i - start;
    if (length && b->Data[start + length - 1] == '\r') length--;
    if (!onLine(user, StrView{b->Data + start, length})) return false;
    start = i + 1;
  }
  if (start) {
    u32 remaining = b->Length - start;
    if (remaining) memmove(b->Data, b->Data + start, remaining);
    b->Length = remaining;
    b->Data[remaining] = '\0';
  }
  return true;
}

// Copy a view into the arena and return a stable view over the copy.
inline StrView StrView_Dup(Arena* arena, StrView s) {
  if (s.Length == 0) return StrView{"", 0};
  char* dst = (char*)ArenaAlloc(arena, s.Length, 1);
  memcpy(dst, s.Data, s.Length);
  return StrView{dst, s.Length};
}

// Arena-owned null-terminated copy, for the C APIs (fopen, opendir, ...) that
// cannot take a length.
inline const char* StrView_DupCStr(Arena* arena, StrView s) {
  char* dst = (char*)ArenaAlloc(arena, s.Length + 1, 1);
  if (s.Length) memcpy(dst, s.Data, s.Length);
  dst[s.Length] = '\0';
  return dst;
}

// CRLF and lone CR become LF. Already-normalized input is borrowed unchanged.
inline StrView StrView_NormalizeLf(Arena* arena, StrView s) {
  if (!s.Length || !memchr(s.Data, '\r', s.Length)) return s;
  char* dst = (char*)ArenaAlloc(arena, s.Length + 1, 1);
  u32 length = 0;
  for (u32 i = 0; i < s.Length; i++) {
    char c = s.Data[i];
    if (c == '\r') {
      if (i + 1 < s.Length && s.Data[i + 1] == '\n') i++;
      c = '\n';
    }
    dst[length++] = c;
  }
  dst[length] = '\0';
  return StrView{dst, length};
}

// Value of one hex digit, or -1 when `c` is not one.
inline int StrView_HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
