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

// Maps one base64url character to its 6-bit value, or -1 if invalid. Static
// helper for Base64UrlDecode.
inline i32 Base64UrlDecodeChar(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-') return 62;
  if (c == '_') return 63;
  return -1;
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

// base64url (RFC 4648 §5) decode. Tolerates '=' padding but does not require
// it (JWT segments are unpadded). Defensive: returns false and leaves *out
// empty on any malformed input (invalid length, invalid character) instead of
// throwing, matching this codebase's no-exceptions convention. Arena-backed.
inline bool Base64UrlDecode(Arena* arena, StrView in, StrView* out) {
  if (out) *out = StrView{};
  if (!arena || !out) return false;

  u32 len = in.Length;
  while (len > 0 && in.Data[len - 1] == '=') len--;
  if (len == 0) {
    *out = StrView{"", 0};
    return true;
  }
  if (len % 4 == 1) return false;  // no valid base64 encoding has this length

  u32 cap = ((len + 3) / 4) * 3 + 1;
  byte* buf = (byte*)ArenaAlloc(arena, cap, 1);
  u32 o = 0;
  u32 i = 0;
  while (i + 4 <= len) {
    i32 a = Base64UrlDecodeChar(in.Data[i]);
    i32 b = Base64UrlDecodeChar(in.Data[i + 1]);
    i32 c = Base64UrlDecodeChar(in.Data[i + 2]);
    i32 d = Base64UrlDecodeChar(in.Data[i + 3]);
    if (a < 0 || b < 0 || c < 0 || d < 0) return false;
    u32 n = ((u32)a << 18) | ((u32)b << 12) | ((u32)c << 6) | (u32)d;
    buf[o++] = (byte)(n >> 16);
    buf[o++] = (byte)(n >> 8);
    buf[o++] = (byte)n;
    i += 4;
  }
  u32 rem = len - i;
  if (rem == 2) {
    i32 a = Base64UrlDecodeChar(in.Data[i]);
    i32 b = Base64UrlDecodeChar(in.Data[i + 1]);
    if (a < 0 || b < 0) return false;
    u32 n = ((u32)a << 18) | ((u32)b << 12);
    buf[o++] = (byte)(n >> 16);
  } else if (rem == 3) {
    i32 a = Base64UrlDecodeChar(in.Data[i]);
    i32 b = Base64UrlDecodeChar(in.Data[i + 1]);
    i32 c = Base64UrlDecodeChar(in.Data[i + 2]);
    if (a < 0 || b < 0 || c < 0) return false;
    u32 n = ((u32)a << 18) | ((u32)b << 12) | ((u32)c << 6);
    buf[o++] = (byte)(n >> 16);
    buf[o++] = (byte)(n >> 8);
  } else if (rem != 0) {
    return false;
  }
  buf[o] = 0;
  *out = StrView{(const char*)buf, o};
  return true;
}
