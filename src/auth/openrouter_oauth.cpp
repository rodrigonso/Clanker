#include "auth/openrouter_oauth.h"

#include <string.h>

#include "crypto/base64.h"
#include "crypto/random.h"
#include "crypto/sha256.h"
#include "json/json.h"
#include "net/http.h"

namespace {

bool IsUnreserved(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
         c == '-' || c == '.' || c == '_' || c == '~';
}

void AppendQueryEscaped(StrBuf* b, StrView s) {
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

void AppendParam(StrBuf* b, const char* name, StrView value) {
  StrBuf_AppendChar(b, memchr(b->Data, '?', b->Length) ? '&' : '?');
  StrBuf_AppendCStr(b, name);
  StrBuf_AppendChar(b, '=');
  AppendQueryEscaped(b, value);
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

StrView OpenRouterOAuth_AuthorizeUrl(Arena* arena, const OAuthPkce* pkce,
                                     StrView callbackUrl) {
  StrBuf url = StrBuf_Make(arena, 256 + callbackUrl.Length * 3);
  StrBuf_AppendCStr(&url, OPENROUTER_OAUTH_AUTHORIZE_URL);
  if (callbackUrl.Length) AppendParam(&url, "callback_url", callbackUrl);
  AppendParam(&url, "code_challenge", pkce->Challenge);
  AppendParam(&url, "code_challenge_method", SVL("S256"));
  AppendParam(&url, "key_label", SVL("Clanker"));
  // OpenRouter only echoes `state` on a redirect.
  if (callbackUrl.Length) AppendParam(&url, "state", pkce->State);
  return StrBuf_View(&url);
}

OAuthCallback OAuthCallback_Parse(Arena* arena, StrView target, StrView expectedState) {
  OAuthCallback result = {};
  StrView path = target;
  StrView query = {};
  for (u32 i = 0; i < target.Length; i++) {
    if (target.Data[i] == '?') {
      path = StrView{target.Data, i};
      query = StrView{target.Data + i + 1, target.Length - i - 1};
      break;
    }
  }
  if (!StrView_EqCStr(path, OPENROUTER_OAUTH_CALLBACK_PATH)) {
    result.Status = OAuthCallbackStatus::WrongPath;
    return result;
  }

  StrView code = {};
  StrView state = {};
  StrView error = {};
  StrView errorDescription = {};
  bool hasState = false;
  u32 start = 0;
  for (u32 i = 0; i <= query.Length; i++) {
    if (i < query.Length && query.Data[i] != '&') continue;
    StrView pair = {query.Data + start, i - start};
    start = i + 1;
    u32 eq = 0;
    while (eq < pair.Length && pair.Data[eq] != '=') eq++;
    StrView key = {pair.Data, eq};
    StrView value = eq < pair.Length ? StrView{pair.Data + eq + 1, pair.Length - eq - 1}
                                     : StrView{};
    if (StrView_EqCStr(key, "code")) {
      code = PercentDecode(arena, value);
    } else if (StrView_EqCStr(key, "state")) {
      state = PercentDecode(arena, value);
      hasState = true;
    } else if (StrView_EqCStr(key, "error")) {
      error = PercentDecode(arena, value);
    } else if (StrView_EqCStr(key, "error_description")) {
      errorDescription = PercentDecode(arena, value);
    }
  }

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

StrView OpenRouterOAuth_ExchangeBody(Arena* arena, StrView code, StrView verifier) {
  StrBuf body = StrBuf_Make(arena, code.Length + verifier.Length + 96);
  JsonWriter w;
  JsonWrite_Begin(&w, &body);
  JsonWrite_String(&w, "code", code);
  JsonWrite_String(&w, "code_verifier", verifier);
  JsonWrite_String(&w, "code_challenge_method", SVL("S256"));
  JsonWrite_End(&w);
  return StrBuf_View(&body);
}

OAuthKeyResult OpenRouterOAuth_ParseExchange(Arena* arena, i32 status, StrView body,
                                             StrView transportError) {
  OAuthKeyResult result = {};
  if (status < 0) {
    result.Error = transportError.Length ? transportError : SVL("the request failed");
    return result;
  }
  JsonValue* root = Json_ParseCopy(arena, body);
  if (status >= 200 && status < 300) {
    StrView key = StrView_Trim(Json_GetStr(root, "key"));
    if (!key.Length) {
      result.Error = SVL("the response did not contain an API key");
      return result;
    }
    result.Ok = true;
    result.Key = key;
    return result;
  }

  JsonValue* error = Json_Get(root, "error");
  StrView detail = Json_IsObject(error) ? Json_GetStr(error, "message")
                   : Json_IsString(error) ? error->Str
                                          : StrView{};
  StrBuf message = StrBuf_Make(arena, detail.Length + 32);
  StrBuf_AppendCStr(&message, "HTTP ");
  StrBuf_AppendI64(&message, status);
  if (detail.Length) {
    StrBuf_AppendCStr(&message, ": ");
    StrBuf_Append(&message, detail);
  }
  result.Error = StrBuf_View(&message);
  return result;
}

OAuthKeyResult OpenRouterOAuth_Exchange(Arena* arena, StrView keysUrl, StrView code,
                                        StrView verifier, volatile i32* abort) {
  HttpHeader headers[] = {{SVL("content-type"), SVL("application/json")}};
  HttpRequest request = {};
  request.Method = SVL("POST");
  request.Url = keysUrl;
  request.Headers = headers;
  request.HeaderCount = 1;
  request.Body = OpenRouterOAuth_ExchangeBody(arena, code, verifier);
  HttpResponse response = Http_Fetch(arena, &request, abort);
  return OpenRouterOAuth_ParseExchange(arena, response.Status, response.Body, response.Error);
}
