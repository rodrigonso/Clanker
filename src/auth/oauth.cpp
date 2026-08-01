#include "auth/oauth.h"

#include "crypto/base64.h"
#include "crypto/pkce.h"
#include "crypto/random.h"
#include "json/json.h"
#include "net/http.h"
#include "net/loopback.h"

namespace {

const OAuthQueryParam kAnthropicExtras[] = {
    {SVL("code"), SVL("true")},
};

const OAuthQueryParam kOpenAiExtras[] = {
    {SVL("id_token_add_organizations"), SVL("true")},
    {SVL("codex_cli_simplified_flow"), SVL("true")},
    {SVL("originator"), SVL("clank")},
};

}  // namespace

// Built-in OAuth providers. Add a row to support a new provider; the token
// application (Bearer/header quirks) lives in that provider's Stream.
static const OAuthConfig CONFIGS[] = {
    {
        SVL("anthropic"),
        SVL("9d1c250a-e61b-44d9-88ed-5944d1962f5e"),
        SVL("https://claude.ai/oauth/authorize"),
        SVL("https://platform.claude.com/v1/oauth/token"),
        SVL("org:create_api_key user:profile user:inference user:sessions:claude_code "
            "user:mcp_servers user:file_upload"),
        SVL("http://localhost:53692/callback"),
        kAnthropicExtras,
        (u32)(sizeof(kAnthropicExtras) / sizeof(kAnthropicExtras[0])),
        OAuthTokenBodyFormat::Json,
    },
    {
        SVL("openai"),
        SVL("app_EMoamEEZ73f0CkXaXp7hrann"),
        SVL("https://auth.openai.com/oauth/authorize"),
        SVL("https://auth.openai.com/oauth/token"),
        SVL("openid profile email offline_access api.connectors.read api.connectors.invoke"),
        SVL("http://localhost:1455/auth/callback"),
        kOpenAiExtras,
        (u32)(sizeof(kOpenAiExtras) / sizeof(kOpenAiExtras[0])),
        OAuthTokenBodyFormat::FormUrlEncoded,
    },
};

const OAuthConfig* OAuth_GetConfig(StrView providerId) {
  for (u32 i = 0; i < sizeof(CONFIGS) / sizeof(CONFIGS[0]); i++) {
    if (StrView_Eq(CONFIGS[i].Id, providerId)) return &CONFIGS[i];
  }
  return nullptr;
}

// Percent-encodes for both URL query strings and
// application/x-www-form-urlencoded bodies (percent-encoded spaces are valid
// in form bodies, so one encoder covers both uses).
static void UrlEncode(StrBuf* b, StrView s) {
  static const char* hex = "0123456789ABCDEF";
  for (u32 i = 0; i < s.Length; i++) {
    char c = s.Data[i];
    bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '-' || c == '.' || c == '_' || c == '~';
    if (unreserved) {
      StrBuf_AppendChar(b, c);
    } else {
      StrBuf_AppendChar(b, '%');
      StrBuf_AppendChar(b, hex[(u8)c >> 4]);
      StrBuf_AppendChar(b, hex[(u8)c & 0xF]);
    }
  }
}

OAuthBegin OAuth_BeginAuthCode(Arena* a, const OAuthConfig* cfg) {
  Pkce p = Pkce_Generate(a);

  // State is independent of the PKCE verifier: it only needs to round-trip
  // through the browser redirect to defend against CSRF, not to be
  // reconstructible from the code_challenge.
  byte stateRaw[32];
  RandomBytes(stateRaw, sizeof(stateRaw));
  StrView state = Base64UrlEncode(a, stateRaw, sizeof(stateRaw));

  StrBuf u = StrBuf_Make(a, 512);
  StrBuf_Append(&u, cfg->AuthorizeUrl);
  char sep = '?';
  for (u32 i = 0; i < cfg->AuthorizeExtraCount; i++) {
    StrBuf_AppendChar(&u, sep);
    sep = '&';
    UrlEncode(&u, cfg->AuthorizeExtras[i].Key);
    StrBuf_AppendChar(&u, '=');
    UrlEncode(&u, cfg->AuthorizeExtras[i].Value);
  }
  StrBuf_AppendChar(&u, sep);
  StrBuf_AppendCStr(&u, "client_id=");
  UrlEncode(&u, cfg->ClientId);
  StrBuf_AppendCStr(&u, "&response_type=code&redirect_uri=");
  UrlEncode(&u, cfg->RedirectUri);
  StrBuf_AppendCStr(&u, "&scope=");
  UrlEncode(&u, cfg->Scopes);
  StrBuf_AppendCStr(&u, "&code_challenge=");
  UrlEncode(&u, p.Challenge);
  StrBuf_AppendCStr(&u, "&code_challenge_method=S256&state=");
  UrlEncode(&u, state);
  return OAuthBegin{StrBuf_View(&u), p.Verifier, state};
}

OAuthRedirect OAuth_ParseRedirect(StrView uri) {
  OAuthRedirect r;
  r.Port = 0;
  r.Path = SVL("/");

  u32 sep = 0;
  bool found = false;
  for (; sep + 3 <= uri.Length; sep++) {
    if (uri.Data[sep] == ':' && uri.Data[sep + 1] == '/' && uri.Data[sep + 2] == '/') {
      found = true;
      break;
    }
  }
  bool https = false;
  u32 i = 0;
  if (found) {
    if (StrView_EqCStr(StrView{uri.Data, sep}, "https")) https = true;
    i = sep + 3;
  }
  while (i < uri.Length && uri.Data[i] != ':' && uri.Data[i] != '/') i++;
  u32 port = https ? 443u : 80u;
  if (i < uri.Length && uri.Data[i] == ':') {
    i++;
    u32 p = 0;
    while (i < uri.Length && uri.Data[i] >= '0' && uri.Data[i] <= '9') {
      p = p * 10 + (u32)(uri.Data[i] - '0');
      i++;
    }
    port = p;
  }
  r.Port = (u16)port;
  if (i < uri.Length && uri.Data[i] == '/') r.Path = StrView{uri.Data + i, uri.Length - i};
  return r;
}

bool OAuth_WaitForCallback(Arena* a, const OAuthConfig* cfg, StrView authUrl, StrView expectedState,
                           OAuthCode* out, volatile i32* abort, StrBuf* err, OAuthReadyFn onReady,
                           void* readyUser) {
  OAuthRedirect rd = OAuth_ParseRedirect(cfg->RedirectUri);
  LoopbackServer srv;
  if (!Loopback_Open(&srv, rd.Port, err)) return false;
  if (onReady) onReady(readyUser, authUrl);
  LoopbackCapture cap = Loopback_WaitForCode(a, &srv, rd.Path, expectedState, abort);
  Loopback_Close(&srv);
  if (!cap.Ok) {
    if (cap.Error.Length)
      StrBuf_Append(err, cap.Error);
    else
      StrBuf_AppendCStr(err, "no authorization code received");
    return false;
  }
  out->Code = cap.Code;
  out->State = cap.State;
  return true;
}

static bool ParseTokenResponse(Arena* a, StrView json, Credential* out,
                               bool allowPartial) {
  JsonValue* j = Json_Parse(a, json);
  if (!Json_IsObject(j)) return false;
  StrView at = Json_GetStr(j, "access_token");
  StrView rt = Json_GetStr(j, "refresh_token");
  StrView id = Json_GetStr(j, "id_token");
  if ((!allowPartial && at.Length == 0) ||
      (at.Length == 0 && rt.Length == 0 && id.Length == 0))
    return false;
  *out = {};
  out->Kind = CredKind::OAuth;
  out->Access = StrView_Dup(a, at);
  out->Refresh = StrView_Dup(a, rt);
  out->IdToken = StrView_Dup(a, id);
  OAuthChatGptIdentity identity = {};
  if ((out->IdToken.Length &&
       OAuth_ParseChatGptIdentity(a, out->IdToken, &identity)) ||
      (out->Access.Length &&
       OAuth_ParseChatGptIdentity(a, out->Access, &identity))) {
    out->AccountId = identity.AccountId;
    out->IsFedRamp = identity.IsFedRamp;
  }
  if (out->Access.Length) {
    i64 expiresIn = Json_GetInt(j, "expires_in", 3600);
    i64 usableSeconds = expiresIn > 300 ? expiresIn - 300 : 0;
    out->ExpiresMs = NowMillis() + (u64)usableSeconds * 1000;
  }
  return true;
}

bool OAuth_ParseTokenResponse(Arena* a, StrView json, Credential* out) {
  return ParseTokenResponse(a, json, out, false);
}

bool OAuth_ParseRefreshTokenResponse(Arena* a, StrView json,
                                     Credential* out) {
  return ParseTokenResponse(a, json, out, true);
}

static bool PostToken(Arena* a, StrView url, StrView body, StrView contentType, Credential* out,
                      StrBuf* err, volatile i32* abort,
                      bool allowPartial) {
  if (abort && *abort) {
    StrBuf_AppendCStr(err, "login cancelled");
    return false;
  }
  HttpHeader h[2] = {{SVL("content-type"), contentType}, {SVL("accept"), SVL("application/json")}};
  HttpRequest req = {};
  req.Method = SVL("POST");
  req.Url = url;
  req.Headers = h;
  req.HeaderCount = 2;
  req.Body = body;
  HttpResponse r = Http_Fetch(a, &req, abort);
  if (abort && *abort) {
    StrBuf_AppendCStr(err, "login cancelled");
    return false;
  }
  if (r.Status < 200 || r.Status >= 300) {
    StrBuf_AppendCStr(err, "token endpoint HTTP ");
    StrBuf_AppendI64(err, r.Status);
    if (r.Body.Length) {
      StrBuf_AppendCStr(err, ": ");
      StrBuf_Append(err, r.Body);
    } else if (r.Error.Length) {
      StrBuf_AppendCStr(err, ": ");
      StrBuf_Append(err, r.Error);
    }
    return false;
  }
  bool parsed = allowPartial
                    ? OAuth_ParseRefreshTokenResponse(a, r.Body, out)
                    : OAuth_ParseTokenResponse(a, r.Body, out);
  if (!parsed) {
    StrBuf_AppendCStr(err, "invalid token response: ");
    StrBuf_Append(err, r.Body);
    return false;
  }
  return true;
}

bool OAuth_ExchangeCode(Arena* a, const OAuthConfig* cfg, StrView code, StrView state, StrView verifier,
                        Credential* out, StrBuf* err, volatile i32* abort) {
  StrBuf body = StrBuf_Make(a, 256);
  if (cfg->TokenFormat == OAuthTokenBodyFormat::FormUrlEncoded) {
    StrBuf_AppendCStr(&body, "grant_type=authorization_code&client_id=");
    UrlEncode(&body, cfg->ClientId);
    StrBuf_AppendCStr(&body, "&code=");
    UrlEncode(&body, code);
    StrBuf_AppendCStr(&body, "&redirect_uri=");
    UrlEncode(&body, cfg->RedirectUri);
    StrBuf_AppendCStr(&body, "&code_verifier=");
    UrlEncode(&body, verifier);
    return PostToken(a, cfg->TokenUrl, StrBuf_View(&body), SVL("application/x-www-form-urlencoded"),
                     out, err, abort, false);
  }
  StrBuf_AppendCStr(&body, "{\"grant_type\":\"authorization_code\",\"client_id\":");
  Json_WriteString(&body, cfg->ClientId);
  StrBuf_AppendCStr(&body, ",\"code\":");
  Json_WriteString(&body, code);
  StrBuf_AppendCStr(&body, ",\"state\":");
  Json_WriteString(&body, state);
  StrBuf_AppendCStr(&body, ",\"redirect_uri\":");
  Json_WriteString(&body, cfg->RedirectUri);
  StrBuf_AppendCStr(&body, ",\"code_verifier\":");
  Json_WriteString(&body, verifier);
  StrBuf_AppendChar(&body, '}');
  return PostToken(a, cfg->TokenUrl, StrBuf_View(&body),
                   SVL("application/json"), out, err, abort, false);
}

bool OAuth_Refresh(Arena* a, const OAuthConfig* cfg, StrView refresh, Credential* out, StrBuf* err,
                   volatile i32* abort) {
  StrBuf body = StrBuf_Make(a, 256);
  if (cfg->TokenFormat == OAuthTokenBodyFormat::FormUrlEncoded) {
    StrBuf_AppendCStr(&body, "grant_type=refresh_token&client_id=");
    UrlEncode(&body, cfg->ClientId);
    StrBuf_AppendCStr(&body, "&refresh_token=");
    UrlEncode(&body, refresh);
    return PostToken(a, cfg->TokenUrl, StrBuf_View(&body), SVL("application/x-www-form-urlencoded"),
                     out, err, abort, true);
  }
  StrBuf_AppendCStr(&body, "{\"grant_type\":\"refresh_token\",\"client_id\":");
  Json_WriteString(&body, cfg->ClientId);
  StrBuf_AppendCStr(&body, ",\"refresh_token\":");
  Json_WriteString(&body, refresh);
  StrBuf_AppendChar(&body, '}');
  return PostToken(a, cfg->TokenUrl, StrBuf_View(&body),
                   SVL("application/json"), out, err, abort, true);
}

bool OAuth_ParseChatGptIdentity(Arena* a, StrView jwt,
                                OAuthChatGptIdentity* out) {
  if (out) *out = {};
  if (!a || !out || jwt.Length == 0) return false;

  // JWT format: header.payload.signature. All three segments must be present
  // and non-empty; we only need the payload.
  u32 firstDot = 0;
  while (firstDot < jwt.Length && jwt.Data[firstDot] != '.') firstDot++;
  if (firstDot == 0 || firstDot >= jwt.Length) return false;
  u32 secondDot = firstDot + 1;
  while (secondDot < jwt.Length && jwt.Data[secondDot] != '.') secondDot++;
  if (secondDot >= jwt.Length || secondDot == firstDot + 1) return false;
  u32 sigLen = jwt.Length - secondDot - 1;
  if (sigLen == 0) return false;

  StrView payloadB64 = StrView{jwt.Data + firstDot + 1, secondDot - firstDot - 1};
  StrView payloadJson;
  if (!Base64UrlDecode(a, payloadB64, &payloadJson)) return false;

  JsonValue* payload = Json_Parse(a, payloadJson);
  if (!Json_IsObject(payload)) return false;
  JsonValue* auth = Json_Get(payload, "https://api.openai.com/auth");
  if (!Json_IsObject(auth)) return false;
  StrView accountId = Json_GetStr(auth, "chatgpt_account_id");
  if (accountId.Length == 0) return false;
  out->AccountId = StrView_Dup(a, accountId);
  out->IsFedRamp =
      Json_GetBool(auth, "chatgpt_account_is_fedramp", false);
  return true;
}

bool OAuth_ParseChatGptAccountId(Arena* a, StrView jwt, StrView* out) {
  if (out) *out = {};
  OAuthChatGptIdentity identity = {};
  if (!out || !OAuth_ParseChatGptIdentity(a, jwt, &identity)) return false;
  *out = identity.AccountId;
  return true;
}
