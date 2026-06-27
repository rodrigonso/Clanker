#include "auth/oauth.h"

#include "crypto/pkce.h"
#include "json/json.h"
#include "net/http.h"
#include "net/loopback.h"

// Built-in OAuth providers. Add a row to support a new provider; the token
// application (Bearer/header quirks) lives in that provider's Stream.
static const OAuthConfig CONFIGS[] = {
    {
        SVL("anthropic"),
        SVL("Anthropic (Claude Pro/Max)"),
        SVL("9d1c250a-e61b-44d9-88ed-5944d1962f5e"),
        SVL("https://claude.ai/oauth/authorize"),
        SVL("https://platform.claude.com/v1/oauth/token"),
        SVL("org:create_api_key user:profile user:inference user:sessions:claude_code "
            "user:mcp_servers user:file_upload"),
        SVL("http://localhost:53692/callback"),
    },
};

const OAuthConfig* OAuth_GetConfig(StrView providerId) {
  for (u32 i = 0; i < sizeof(CONFIGS) / sizeof(CONFIGS[0]); i++) {
    if (StrView_Eq(CONFIGS[i].Id, providerId)) return &CONFIGS[i];
  }
  return nullptr;
}

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
  StrBuf u = StrBuf_Make(a, 512);
  StrBuf_Append(&u, cfg->AuthorizeUrl);
  StrBuf_AppendCStr(&u, "?code=true&client_id=");
  UrlEncode(&u, cfg->ClientId);
  StrBuf_AppendCStr(&u, "&response_type=code&redirect_uri=");
  UrlEncode(&u, cfg->RedirectUri);
  StrBuf_AppendCStr(&u, "&scope=");
  UrlEncode(&u, cfg->Scopes);
  StrBuf_AppendCStr(&u, "&code_challenge=");
  UrlEncode(&u, p.Challenge);
  StrBuf_AppendCStr(&u, "&code_challenge_method=S256&state=");
  UrlEncode(&u, p.Verifier);
  return OAuthBegin{StrBuf_View(&u), p.Verifier};
}

OAuthRedirect OAuth_ParseRedirect(StrView uri) {
  OAuthRedirect r;
  r.Host = StrView{nullptr, 0};
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
  u32 hostStart = i;
  while (i < uri.Length && uri.Data[i] != ':' && uri.Data[i] != '/') i++;
  r.Host = StrView{uri.Data + hostStart, i - hostStart};
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

bool OAuth_ParseTokenResponse(Arena* a, StrView json, Credential* out) {
  JsonValue* j = Json_Parse(a, json);
  if (!Json_IsObject(j)) return false;
  StrView at = Json_GetStr(j, "access_token");
  if (at.Length == 0) return false;
  *out = {};
  out->Kind = CredKind::OAuth;
  out->Access = StrView_Dup(a, at);
  out->Refresh = StrView_Dup(a, Json_GetStr(j, "refresh_token"));
  i64 expiresIn = Json_GetInt(j, "expires_in", 3600);
  out->ExpiresMs = NowMillis() + (u64)expiresIn * 1000 - 5 * 60 * 1000;  // 5-min safety margin
  return true;
}

static bool PostToken(Arena* a, StrView url, StrView body, Credential* out, StrBuf* err) {
  HttpHeader h[2] = {{SVL("content-type"), SVL("application/json")}, {SVL("accept"), SVL("application/json")}};
  HttpRequest req = {};
  req.Method = SVL("POST");
  req.Url = url;
  req.Headers = h;
  req.HeaderCount = 2;
  req.Body = body;
  HttpResponse r = Http_Fetch(a, &req, nullptr);
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
  if (!OAuth_ParseTokenResponse(a, r.Body, out)) {
    StrBuf_AppendCStr(err, "invalid token response: ");
    StrBuf_Append(err, r.Body);
    return false;
  }
  return true;
}

bool OAuth_ExchangeCode(Arena* a, const OAuthConfig* cfg, StrView code, StrView state, StrView verifier,
                        Credential* out, StrBuf* err) {
  StrBuf body = StrBuf_Make(a, 256);
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
  return PostToken(a, cfg->TokenUrl, StrBuf_View(&body), out, err);
}

bool OAuth_Refresh(Arena* a, const OAuthConfig* cfg, StrView refresh, Credential* out, StrBuf* err) {
  StrBuf body = StrBuf_Make(a, 256);
  StrBuf_AppendCStr(&body, "{\"grant_type\":\"refresh_token\",\"client_id\":");
  Json_WriteString(&body, cfg->ClientId);
  StrBuf_AppendCStr(&body, ",\"refresh_token\":");
  Json_WriteString(&body, refresh);
  StrBuf_AppendChar(&body, '}');
  return PostToken(a, cfg->TokenUrl, StrBuf_View(&body), out, err);
}
