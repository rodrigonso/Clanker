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

inline StrView StrBuf_View(StrBuf* b) { return StrView{b->Data, b->Length}; }
inline const char* StrBuf_CStr(StrBuf* b) { return b->Data; }

inline void StrBuf_Clear(StrBuf* b) {
  b->Length = 0;
  b->Data[0] = '\0';
}

// Copy a view into the arena and return a stable view over the copy.
inline StrView StrView_Dup(Arena* arena, StrView s) {
  if (s.Length == 0) return StrView{"", 0};
  char* dst = (char*)ArenaAlloc(arena, s.Length, 1);
  memcpy(dst, s.Data, s.Length);
  return StrView{dst, s.Length};
}
