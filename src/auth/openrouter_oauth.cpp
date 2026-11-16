#include "auth/openrouter_oauth.h"

#include "json/json.h"
#include "net/http.h"

StrView OpenRouterOAuth_AuthorizeUrl(Arena* arena, const OAuthPkce* pkce,
                                     StrView callbackUrl) {
  StrBuf url = StrBuf_Make(arena, 256 + callbackUrl.Length * 3);
  StrBuf_AppendCStr(&url, OPENROUTER_OAUTH_AUTHORIZE_URL);
  if (callbackUrl.Length) OAuthQuery_Append(&url, "callback_url", callbackUrl);
  OAuthQuery_Append(&url, "code_challenge", pkce->Challenge);
  OAuthQuery_Append(&url, "code_challenge_method", SVL("S256"));
  OAuthQuery_Append(&url, "key_label", SVL("Clanker"));
  // OpenRouter only echoes `state` on a redirect.
  if (callbackUrl.Length) OAuthQuery_Append(&url, "state", pkce->State);
  return StrBuf_View(&url);
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
