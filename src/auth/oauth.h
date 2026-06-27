#pragma once
#include "auth/credential.h"
#include "base/arena.h"
#include "base/str.h"

// Generic OAuth (authorization-code + PKCE). Provider-specific differences are
// just config values; how the resulting token is applied to a request lives in
// each provider's Stream, not here.
struct OAuthConfig {
  StrView Id;
  StrView Name;
  StrView ClientId;
  StrView AuthorizeUrl;
  StrView TokenUrl;
  StrView Scopes;
  StrView RedirectUri;
};

// Returns nullptr if the provider has no OAuth config.
const OAuthConfig* OAuth_GetConfig(StrView providerId);

struct OAuthBegin {
  StrView AuthUrl;   // open this in a browser
  StrView Verifier;  // keep for the exchange step
};
OAuthBegin OAuth_BeginAuthCode(Arena* arena, const OAuthConfig* cfg);

// Authorization code + state captured from the OAuth redirect.
struct OAuthCode {
  StrView Code;
  StrView State;
};

// Parsed redirect URI ("http://host:port/path"). Host/Path are views into the
// input string (stable for the config's lifetime).
struct OAuthRedirect {
  StrView Host;
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
// Requires ma_loopback. `abort` may be null.
bool OAuth_WaitForCallback(Arena* arena, const OAuthConfig* cfg, StrView authUrl, StrView expectedState,
                           OAuthCode* out, volatile i32* abort, StrBuf* err, OAuthReadyFn onReady,
                           void* readyUser);

// Parse a token-endpoint JSON response into an OAuth credential (testable offline).
bool OAuth_ParseTokenResponse(Arena* arena, StrView json, Credential* out);

// Network calls (require ma_http). Fill `out` and return true, else write `err`.
bool OAuth_ExchangeCode(Arena* arena, const OAuthConfig* cfg, StrView code, StrView state, StrView verifier,
                        Credential* out, StrBuf* err);
bool OAuth_Refresh(Arena* arena, const OAuthConfig* cfg, StrView refresh, Credential* out, StrBuf* err);
