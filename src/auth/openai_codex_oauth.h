#pragma once
#include "auth/oauth.h"
#include "base/arena.h"
#include "base/str.h"

// ChatGPT sign-in for the Codex backend, the flow the Codex CLI and pi use:
// authorization code with PKCE against auth.openai.com, redirected to a fixed
// loopback port, yielding a short-lived access token and a refresh token
// scoped to the user's ChatGPT plan. Backend requests also name the ChatGPT
// account, which is read from the access token's claims.
//
// Everything except _Exchange and _Refresh is pure and offline-testable.

#define OPENAI_CODEX_OAUTH_CLIENT_ID "app_EMoamEEZ73f0CkXaXp7hrann"
#define OPENAI_CODEX_OAUTH_AUTHORIZE_URL "https://auth.openai.com/oauth/authorize"
#define OPENAI_CODEX_OAUTH_TOKEN_URL "https://auth.openai.com/oauth/token"
#define OPENAI_CODEX_OAUTH_SCOPE "openid profile email offline_access"
// The client is registered with exactly this redirect, so the port is fixed
// rather than OS-assigned and is shared with the Codex CLI's own sign-in.
#define OPENAI_CODEX_OAUTH_CALLBACK_PORT 1455
#define OPENAI_CODEX_OAUTH_CALLBACK_PATH "/auth/callback"
#define OPENAI_CODEX_OAUTH_REDIRECT_URI "http://localhost:1455/auth/callback"

StrView OpenAICodexOAuth_AuthorizeUrl(Arena* arena, const OAuthPkce* pkce);
StrView OpenAICodexOAuth_ExchangeBody(Arena* arena, StrView code, StrView verifier);
StrView OpenAICodexOAuth_RefreshBody(Arena* arena, StrView refreshToken);

struct OpenAICodexTokens {
  bool Ok;
  StrView Access;
  StrView Refresh;
  StrView AccountId;
  u64 ExpiresAtMs;  // Unix epoch milliseconds
  StrView Error;
};

// Parses a token-endpoint response. `nowMs` anchors `expires_in`; without one
// the access token's own `exp` claim is used. A refresh response that does not
// rotate the refresh token keeps `previousRefresh`.
OpenAICodexTokens OpenAICodexOAuth_ParseTokens(Arena* arena, i32 status, StrView body,
                                               StrView transportError,
                                               StrView previousRefresh, u64 nowMs);

// The `chatgpt_account_id` claim of an access token, or empty.
StrView OpenAICodexOAuth_AccountId(Arena* arena, StrView accessToken);
// The access token's `exp` claim in milliseconds, or 0.
u64 OpenAICodexOAuth_TokenExpiry(Arena* arena, StrView accessToken);

// Reads what a user pastes when the redirect cannot reach Clanker: the full
// redirect URL from the address bar, "code#state", a query string, or the bare
// code. False with `*error` set on a denial, a state from another attempt, or
// no code at all.
bool OpenAICodexOAuth_ParsePastedInput(Arena* arena, StrView input, StrView expectedState,
                                       StrView* code, StrView* error);

OpenAICodexTokens OpenAICodexOAuth_Exchange(Arena* arena, StrView code, StrView verifier,
                                            volatile i32* abort);
OpenAICodexTokens OpenAICodexOAuth_Refresh(Arena* arena, StrView refreshToken,
                                           volatile i32* abort);
