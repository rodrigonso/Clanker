#include "auth/oauth.h"

#include <string.h>

#include "crypto/base64.h"
#include "crypto/random.h"
#include "crypto/sha256.h"

namespace {

bool IsUnreserved(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
         c == '-' || c == '.' || c == '_' || c == '~';
}

void AppendEscaped(StrBuf* b, StrView s) {
  static const char* hex = "0123456789ABCDEF";
  for (u32 i = 0; i < s.Length; i++) {
    char c = s.Data[i];
    if (IsUnreserved(c)) {
      StrBuf_AppendChar(b, c);
    } else {
      StrBuf_AppendChar(b, '%');
      StrBuf_AppendChar(b, hex[((u8)c >> 4) & 15]);
      StrBuf_AppendChar(b, hex[(u8)c & 15]);
    }
  }
}

StrView PercentDecode(Arena* arena, StrView s) {
  StrBuf out = StrBuf_Make(arena, s.Length + 1);
  for (u32 i = 0; i < s.Length; i++) {
    char c = s.Data[i];
    if (c == '+') {
      StrBuf_AppendChar(&out, ' ');
    } else if (c == '%' && i + 2 < s.Length && StrView_HexDigit(s.Data[i + 1]) >= 0 &&
               StrView_HexDigit(s.Data[i + 2]) >= 0) {
      StrBuf_AppendChar(&out, (char)((StrView_HexDigit(s.Data[i + 1]) << 4) |
                                     StrView_HexDigit(s.Data[i + 2])));
      i += 2;
    } else {
      StrBuf_AppendChar(&out, c);
    }
  }
  return StrBuf_View(&out);
}

StrView RandomToken(Arena* arena, u32 bytes) {
  byte buf[32];
  FFI(bytes > sizeof(buf));
  RandomBytes(buf, bytes);
  return Base64UrlEncode(arena, buf, bytes);
}

}  // namespace

StrView OAuthPkce_Challenge(Arena* arena, StrView verifier) {
  byte digest[SHA256_DIGEST_SIZE];
  Sha256((const byte*)verifier.Data, verifier.Length, digest);
  return Base64UrlEncode(arena, digest, SHA256_DIGEST_SIZE);
}

OAuthPkce OAuthPkce_Create(Arena* arena) {
  OAuthPkce pkce = {};
  // 32 bytes encode to 43 characters, the RFC 7636 minimum verifier length.
  pkce.Verifier = RandomToken(arena, 32);
  pkce.Challenge = OAuthPkce_Challenge(arena, pkce.Verifier);
  pkce.State = RandomToken(arena, 16);
  return pkce;
}

void OAuthQuery_Append(StrBuf* url, const char* name, StrView value) {
  StrBuf_AppendChar(url, memchr(url->Data, '?', url->Length) ? '&' : '?');
  StrBuf_AppendCStr(url, name);
  StrBuf_AppendChar(url, '=');
  AppendEscaped(url, value);
}

void OAuthForm_Append(StrBuf* body, const char* name, StrView value) {
  if (body->Length) StrBuf_AppendChar(body, '&');
  StrBuf_AppendCStr(body, name);
  StrBuf_AppendChar(body, '=');
  AppendEscaped(body, value);
}

bool OAuthQuery_Find(Arena* arena, StrView query, const char* name, StrView* out) {
  *out = {};
  u32 start = 0;
  for (u32 i = 0; i <= query.Length; i++) {
    if (i < query.Length && query.Data[i] != '&') continue;
    StrView pair = {query.Data + start, i - start};
    start = i + 1;
    u32 eq = 0;
    while (eq < pair.Length && pair.Data[eq] != '=') eq++;
    if (!StrView_EqCStr(StrView{pair.Data, eq}, name)) continue;
    StrView value = eq < pair.Length ? StrView{pair.Data + eq + 1, pair.Length - eq - 1}
                                     : StrView{};
    *out = PercentDecode(arena, value);
    return true;
  }
  return false;
}

OAuthCallback OAuthCallback_Parse(Arena* arena, StrView target, StrView path,
                                  StrView expectedState) {
  OAuthCallback result = {};
  StrView targetPath = target;
  StrView query = {};
  for (u32 i = 0; i < target.Length; i++) {
    if (target.Data[i] == '?') {
      targetPath = StrView{target.Data, i};
      query = StrView{target.Data + i + 1, target.Length - i - 1};
      break;
    }
  }
  if (!StrView_Eq(targetPath, path)) {
    result.Status = OAuthCallbackStatus::WrongPath;
    return result;
  }

  StrView code = {};
  StrView state = {};
  StrView error = {};
  StrView errorDescription = {};
  OAuthQuery_Find(arena, query, "code", &code);
  bool hasState = OAuthQuery_Find(arena, query, "state", &state);
  OAuthQuery_Find(arena, query, "error", &error);
  OAuthQuery_Find(arena, query, "error_description", &errorDescription);

  // A denial carries no state, so it cannot be tied to this request; it ends
  // the attempt but grants nothing.
  if (error.Length) {
    result.Status = OAuthCallbackStatus::Denied;
    result.Error = errorDescription.Length ? errorDescription : error;
    return result;
  }
  if (!hasState || !StrView_Eq(state, expectedState)) {
    result.Status = OAuthCallbackStatus::StateMismatch;
    return result;
  }
  if (!code.Length) {
    result.Status = OAuthCallbackStatus::MissingCode;
    return result;
  }
  result.Status = OAuthCallbackStatus::Code;
  result.Code = code;
  return result;
}
