#pragma once
#include "auth/credential.h"
#include "base/arena.h"
#include "base/str.h"

// Generic OAuth (authorization-code + PKCE). Provider-specific differences are
// just config values; how the resulting token is applied to a request lives in
// each provider's Stream, not here.

// Extra "key=value" pair appended to the authorize URL's query string. Used
// for provider-specific authorize-time flags (e.g. Anthropic's `code=true` or
// OpenAI/Codex's `id_token_add_organizations`/`codex_cli_simplified_flow`/
// `originator` flow flags).
struct OAuthQueryParam {
  StrView Key;
  StrView Value;
};

// Body encoding used for the token endpoint (code exchange + refresh) requests.
enum class OAuthTokenBodyFormat : u8 { Json, FormUrlEncoded };

struct OAuthConfig {
  StrView Id;
  StrView ClientId;
  StrView AuthorizeUrl;
  StrView TokenUrl;
  StrView Scopes;
  StrView RedirectUri;
  const OAuthQueryParam* AuthorizeExtras;  // appended to the authorize URL, in order
  u32 AuthorizeExtraCount;
  OAuthTokenBodyFormat TokenFormat;  // exchange/refresh request body encoding
};

// Returns nullptr if the provider has no OAuth config.
const OAuthConfig* OAuth_GetConfig(StrView providerId);

struct OAuthBegin {
  StrView AuthUrl;   // open this in a browser
  StrView Verifier;  // PKCE code_verifier; keep for the exchange step
  StrView State;     // independent random state; keep for the exchange/redirect
};
OAuthBegin OAuth_BeginAuthCode(Arena* arena, const OAuthConfig* cfg);

// Authorization code + state captured from the OAuth redirect.
struct OAuthCode {
  StrView Code;
  StrView State;
};

// Parsed redirect URI ("http://host:port/path"). Path is a view into the input
// string (stable for the config's lifetime).
struct OAuthRedirect {
  u16 Port;
  StrView Path;  // e.g. "/callback"
};
OAuthRedirect OAuth_ParseRedirect(StrView uri);

// Fired once the loopback server is listening, before we block waiting for the
// browser redirect. The app uses it to open the browser / print instructions.
typedef void (*OAuthReadyFn)(void* user, StrView authUrl);

// Run a loopback OAuth callback capture: opens a local server on the config's
// redirect port (127.0.0.1), invokes `onReady` (open browser / print), then
// blocks until the code arrives, the user aborts (*abort != 0), or an error
// occurs. On success fills `out` and returns true; otherwise writes `err`.
// Requires clank_loopback. `abort` may be null.
bool OAuth_WaitForCallback(Arena* arena, const OAuthConfig* cfg, StrView authUrl, StrView expectedState,
                           OAuthCode* out, volatile i32* abort, StrBuf* err, OAuthReadyFn onReady,
                           void* readyUser);

// Parse a token-endpoint JSON response into an OAuth credential (testable offline).
bool OAuth_ParseTokenResponse(Arena* arena, StrView json, Credential* out);
// Refresh responses may rotate any subset of access, refresh, and identity
// tokens. Returns false only when the response is malformed or has no tokens.
bool OAuth_ParseRefreshTokenResponse(Arena* arena, StrView json,
                                     Credential* out);

struct OAuthChatGptIdentity {
  StrView AccountId;
  bool IsFedRamp;
};

bool OAuth_ParseChatGptIdentity(Arena* arena, StrView jwt,
                                OAuthChatGptIdentity* out);

// Extract the ChatGPT account id (the "chatgpt_account_id" field of the
// "https://api.openai.com/auth" claim) from a JWT access/id token's payload,
// without verifying the signature. Returns false, leaving *out empty, if the
// token isn't a well-formed 3-segment JWT, its payload segment isn't valid
// base64url, the decoded payload isn't a JSON object, or the claim/field is
// absent or not a string.
bool OAuth_ParseChatGptAccountId(Arena* arena, StrView jwt, StrView* out);

// Network calls (require clank_http). Fill `out` and return true, else write
// `err`. Optional abort is polled by the HTTP transport.
bool OAuth_ExchangeCode(Arena* arena, const OAuthConfig* cfg, StrView code, StrView state, StrView verifier,
                        Credential* out, StrBuf* err, volatile i32* abort = nullptr);
bool OAuth_Refresh(Arena* arena, const OAuthConfig* cfg, StrView refresh, Credential* out,
                   StrBuf* err, volatile i32* abort = nullptr);
