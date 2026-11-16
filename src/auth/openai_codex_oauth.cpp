#include "auth/openai_codex_oauth.h"

#include "crypto/base64.h"
#include "json/json.h"
#include "net/http.h"

namespace {

// Clanker identifies itself to the backend the way pi and the Codex CLI do.
const StrView ORIGINATOR = SVL("clanker");

u32 FindChar(StrView s, char c) {
  for (u32 i = 0; i < s.Length; i++)
    if (s.Data[i] == c) return i;
  return s.Length;
}

JsonValue* AccessTokenClaims(Arena* arena, StrView token) {
  u32 first = FindChar(token, '.');
  if (first == token.Length) return nullptr;
  StrView rest = {token.Data + first + 1, token.Length - first - 1};
  u32 second = FindChar(rest, '.');
  if (second == rest.Length) return nullptr;
  StrView payload = {};
  if (!Base64UrlDecode(arena, StrView{rest.Data, second}, &payload)) return nullptr;
  JsonValue* claims = Json_Parse(arena, payload);
  return Json_IsObject(claims) ? claims : nullptr;
}

StrView TokenErrorDetail(JsonValue* root) {
  JsonValue* error = Json_Get(root, "error");
  StrView detail = Json_IsObject(error) ? Json_GetStr(error, "message") : StrView{};
  if (!detail.Length) detail = Json_GetStr(root, "error_description");
  if (!detail.Length && Json_IsObject(error)) detail = Json_GetStr(error, "code");
  if (!detail.Length && Json_IsString(error)) detail = error->Str;
  return detail;
}

OpenAICodexTokens Failure(StrView error) {
  OpenAICodexTokens result = {};
  result.Error = error;
  return result;
}

OpenAICodexTokens PostTokenForm(Arena* arena, StrView body, StrView previousRefresh,
                                volatile i32* abort) {
  HttpHeader headers[] = {
      {SVL("content-type"), SVL("application/x-www-form-urlencoded")},
      {SVL("accept"), SVL("application/json")},
  };
  HttpRequest request = {};
  request.Method = SVL("POST");
  request.Url = SVL(OPENAI_CODEX_OAUTH_TOKEN_URL);
  request.Headers = headers;
  request.HeaderCount = (u32)(sizeof(headers) / sizeof(headers[0]));
  request.Body = body;
  request.TimeoutMs = 60 * 1000;
  request.MaxResponseBytes = 1u << 20;
  HttpResponse response = Http_Fetch(arena, &request, abort);
  return OpenAICodexOAuth_ParseTokens(arena, response.Status, response.Body,
                                      response.Error, previousRefresh, NowMillis());
}

}  // namespace

StrView OpenAICodexOAuth_AuthorizeUrl(Arena* arena, const OAuthPkce* pkce) {
  StrBuf url = StrBuf_Make(arena, 512);
  StrBuf_AppendCStr(&url, OPENAI_CODEX_OAUTH_AUTHORIZE_URL);
  OAuthQuery_Append(&url, "response_type", SVL("code"));
  OAuthQuery_Append(&url, "client_id", SVL(OPENAI_CODEX_OAUTH_CLIENT_ID));
  OAuthQuery_Append(&url, "redirect_uri", SVL(OPENAI_CODEX_OAUTH_REDIRECT_URI));
  OAuthQuery_Append(&url, "scope", SVL(OPENAI_CODEX_OAUTH_SCOPE));
  OAuthQuery_Append(&url, "code_challenge", pkce->Challenge);
  OAuthQuery_Append(&url, "code_challenge_method", SVL("S256"));
  OAuthQuery_Append(&url, "state", pkce->State);
  OAuthQuery_Append(&url, "id_token_add_organizations", SVL("true"));
  OAuthQuery_Append(&url, "codex_cli_simplified_flow", SVL("true"));
  OAuthQuery_Append(&url, "originator", ORIGINATOR);
  return StrBuf_View(&url);
}

StrView OpenAICodexOAuth_ExchangeBody(Arena* arena, StrView code, StrView verifier) {
  StrBuf body = StrBuf_Make(arena, 192 + (code.Length + verifier.Length) * 3);
  OAuthForm_Append(&body, "grant_type", SVL("authorization_code"));
  OAuthForm_Append(&body, "client_id", SVL(OPENAI_CODEX_OAUTH_CLIENT_ID));
  OAuthForm_Append(&body, "code", code);
  OAuthForm_Append(&body, "code_verifier", verifier);
  OAuthForm_Append(&body, "redirect_uri", SVL(OPENAI_CODEX_OAUTH_REDIRECT_URI));
  return StrBuf_View(&body);
}

StrView OpenAICodexOAuth_RefreshBody(Arena* arena, StrView refreshToken) {
  StrBuf body = StrBuf_Make(arena, 96 + refreshToken.Length * 3);
  OAuthForm_Append(&body, "grant_type", SVL("refresh_token"));
  OAuthForm_Append(&body, "refresh_token", refreshToken);
  OAuthForm_Append(&body, "client_id", SVL(OPENAI_CODEX_OAUTH_CLIENT_ID));
  return StrBuf_View(&body);
}

StrView OpenAICodexOAuth_AccountId(Arena* arena, StrView accessToken) {
  JsonValue* claims = AccessTokenClaims(arena, accessToken);
  JsonValue* auth = Json_Get(claims, "https://api.openai.com/auth");
  return Json_GetStr(auth, "chatgpt_account_id");
}

u64 OpenAICodexOAuth_TokenExpiry(Arena* arena, StrView accessToken) {
  JsonValue* claims = AccessTokenClaims(arena, accessToken);
  f64 exp = Json_GetNum(claims, "exp", 0);
  return exp > 0 && exp < 9.0e12 ? (u64)exp * 1000 : 0;
}

OpenAICodexTokens OpenAICodexOAuth_ParseTokens(Arena* arena, i32 status, StrView body,
                                               StrView transportError,
                                               StrView previousRefresh, u64 nowMs) {
  if (status < 0)
    return Failure(transportError.Length ? transportError : SVL("the request failed"));
  JsonValue* root = Json_ParseCopy(arena, body);
  if (status < 200 || status >= 300) {
    StrView detail = TokenErrorDetail(root);
    StrBuf message = StrBuf_Make(arena, detail.Length + 32);
    StrBuf_AppendCStr(&message, "HTTP ");
    StrBuf_AppendI64(&message, status);
    if (detail.Length) {
      StrBuf_AppendCStr(&message, ": ");
      StrBuf_Append(&message, detail);
    }
    return Failure(StrBuf_View(&message));
  }

  StrView access = Json_GetStr(root, "access_token");
  if (!access.Length) return Failure(SVL("the response did not contain an access token"));
  StrView refresh = Json_GetStr(root, "refresh_token");
  if (!refresh.Length) refresh = previousRefresh;
  if (!refresh.Length) return Failure(SVL("the response did not contain a refresh token"));
  StrView accountId = OpenAICodexOAuth_AccountId(arena, access);
  if (!accountId.Length)
    return Failure(SVL("the access token does not identify a ChatGPT account"));

  f64 expiresIn = Json_GetNum(root, "expires_in", 0);
  u64 expiresAt = expiresIn > 0 && expiresIn < 9.0e9
                      ? nowMs + (u64)expiresIn * 1000
                      : OpenAICodexOAuth_TokenExpiry(arena, access);
  if (!expiresAt) return Failure(SVL("the response did not say when the token expires"));

  OpenAICodexTokens result = {};
  result.Ok = true;
  result.Access = access;
  result.Refresh = StrView_Dup(arena, refresh);
  result.AccountId = accountId;
  result.ExpiresAtMs = expiresAt;
  return result;
}

bool OpenAICodexOAuth_ParsePastedInput(Arena* arena, StrView input, StrView expectedState,
                                       StrView* code, StrView* error) {
  *code = {};
  *error = {};
  StrView value = StrView_Trim(input);
  StrView state = {};
  bool hasState = false;
  StrView query = {};
  bool isQuery = false;
  if (StrView_Contains(value, "://")) {
    u32 q = FindChar(value, '?');
    StrView afterQuery = q < value.Length
                             ? StrView{value.Data + q + 1, value.Length - q - 1}
                             : StrView{};
    query = StrView{afterQuery.Data, FindChar(afterQuery, '#')};
    isQuery = true;
  } else if (FindChar(value, '#') < value.Length) {
    u32 hash = FindChar(value, '#');
    *code = StrView{value.Data, hash};
    state = StrView{value.Data + hash + 1, value.Length - hash - 1};
    hasState = true;
  } else if (StrView_Contains(value, "code=")) {
    query = value.Length && value.Data[0] == '?'
                ? StrView{value.Data + 1, value.Length - 1}
                : value;
    isQuery = true;
  } else {
    *code = value;
  }

  if (isQuery) {
    StrView denied = {};
    StrView description = {};
    if (OAuthQuery_Find(arena, query, "error", &denied) && denied.Length) {
      OAuthQuery_Find(arena, query, "error_description", &description);
      StrView reason = description.Length ? description : denied;
      StrBuf message = StrBuf_Make(arena, reason.Length + 32);
      StrBuf_AppendCStr(&message, "sign-in was denied: ");
      StrBuf_Append(&message, reason);
      *error = StrBuf_View(&message);
      *code = {};
      return false;
    }
    OAuthQuery_Find(arena, query, "code", code);
    hasState = OAuthQuery_Find(arena, query, "state", &state);
  }

  if (hasState && !StrView_Eq(state, expectedState)) {
    *code = {};
    *error = SVL("this authorization belongs to a different sign-in attempt");
    return false;
  }
  if (!code->Length) {
    *error = SVL("no authorization code was found");
    return false;
  }
  return true;
}

OpenAICodexTokens OpenAICodexOAuth_Exchange(Arena* arena, StrView code, StrView verifier,
                                            volatile i32* abort) {
  return PostTokenForm(arena, OpenAICodexOAuth_ExchangeBody(arena, code, verifier),
                       StrView{}, abort);
}

OpenAICodexTokens OpenAICodexOAuth_Refresh(Arena* arena, StrView refreshToken,
                                           volatile i32* abort) {
  return PostTokenForm(arena, OpenAICodexOAuth_RefreshBody(arena, refreshToken),
                       refreshToken, abort);
}
