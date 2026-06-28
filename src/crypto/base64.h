#pragma once
#include "base/arena.h"
#include "base/str.h"

// Standard base64 (RFC 4648 §4) WITH padding. Arena-allocated, null-terminated.
// Used for image content data sent to the Anthropic API.
inline StrView Base64Encode(Arena* arena, const byte* data, u32 len) {
  static const char* T =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
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
    buf[o++] = '=';
    buf[o++] = '=';
  } else if (rem == 2) {
    u32 n = ((u32)data[i] << 16) | ((u32)data[i + 1] << 8);
    buf[o++] = T[(n >> 18) & 63];
    buf[o++] = T[(n >> 12) & 63];
    buf[o++] = T[(n >> 6) & 63];
    buf[o++] = '=';
  }
  buf[o] = '\0';
  return StrView{buf, o};
}

// base64url (RFC 4648 §5) without padding. Arena-allocated, null-terminated.
inline StrView Base64UrlEncode(Arena* arena, const byte* data, u32 len) {
  static const char* T =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
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
  } else if (rem == 2) {
    u32 n = ((u32)data[i] << 16) | ((u32)data[i + 1] << 8);
    buf[o++] = T[(n >> 18) & 63];
    buf[o++] = T[(n >> 12) & 63];
    buf[o++] = T[(n >> 6) & 63];
  }
  buf[o] = '\0';
  return StrView{buf, o};
}
