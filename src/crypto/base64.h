#pragma once
#include "base/arena.h"
#include "base/str.h"

inline StrView Base64EncodeWith(Arena* arena, const byte* data, u32 len,
                                const char* T, bool pad) {
  u32 cap = ((len + 2) / 3) * 4 + 1;
  char* buf = (char*)ArenaAlloc(arena, cap, 1);
  u32 o = 0;
  u32 i = 0;
  while (i + 3 <= len) {
    u32 n = ((u32)data[i] << 16) | ((u32)data[i + 1] << 8) | (u32)data[i + 2];
    buf[o++] = T[(n >> 18) & 63];
    buf[o++] = T[(n >> 12) & 63];
    buf[o++] = T[(n >> 6) & 63];
    buf[o++] = T[n & 63];
    i += 3;
  }
  u32 rem = len - i;
  if (rem == 1) {
    u32 n = (u32)data[i] << 16;
    buf[o++] = T[(n >> 18) & 63];
    buf[o++] = T[(n >> 12) & 63];
    if (pad) {
      buf[o++] = '=';
      buf[o++] = '=';
    }
  } else if (rem == 2) {
    u32 n = ((u32)data[i] << 16) | ((u32)data[i + 1] << 8);
    buf[o++] = T[(n >> 18) & 63];
    buf[o++] = T[(n >> 12) & 63];
    buf[o++] = T[(n >> 6) & 63];
    if (pad) buf[o++] = '=';
  }
  buf[o] = '\0';
  return StrView{buf, o};
}

// Standard base64 (RFC 4648 §4) WITH padding. Arena-allocated, null-terminated.
// Used for inline image content in provider requests.
inline StrView Base64Encode(Arena* arena, const byte* data, u32 len) {
  return Base64EncodeWith(
      arena, data, len,
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/", true);
}

// URL-safe base64 (RFC 4648 §5) WITHOUT padding, as OAuth PKCE requires.
inline StrView Base64UrlEncode(Arena* arena, const byte* data, u32 len) {
  return Base64EncodeWith(
      arena, data, len,
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_", false);
}
