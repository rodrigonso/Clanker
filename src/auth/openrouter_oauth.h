#pragma once
#include "base/arena.h"
#include "base/str.h"

// OpenRouter's OAuth PKCE flow (https://openrouter.ai/docs/guides/overview/auth/oauth).
// The user approves Clanker in a browser and OpenRouter mints an ordinary API
// key for their account, so the result is stored and used exactly like a key.
//
// Everything except OpenRouterOAuth_Exchange is pure and offline-testable.

#define OPENROUTER_OAUTH_AUTHORIZE_URL "https://openrouter.ai/auth"
#define OPENROUTER_OAUTH_CALLBACK_PATH "/callback"

struct OAuthPkce {
  StrView Verifier;
  StrView Challenge;  // base64url(SHA-256(Verifier)), method S256
  StrView State;
};

OAuthPkce OAuthPkce_Create(Arena* arena);
StrView OAuthPkce_Challenge(Arena* arena, StrView verifier);

// An empty `callbackUrl` requests OpenRouter's headless mode, where the page
// shows the code for the user to paste instead of redirecting.
StrView OpenRouterOAuth_AuthorizeUrl(Arena* arena, const OAuthPkce* pkce,
                                     StrView callbackUrl);

enum class OAuthCallbackStatus : u8 { Code, WrongPath, StateMismatch, Denied, MissingCode };

struct OAuthCallback {
  OAuthCallbackStatus Status;
  StrView Code;
  StrView Error;  // provider-supplied reason when Denied
};

// Classifies a loopback request-target such as "/callback?code=...&state=...".
OAuthCallback OAuthCallback_Parse(Arena* arena, StrView target, StrView expectedState);

struct OAuthKeyResult {
  bool Ok;
  StrView Key;
  StrView Error;
};

StrView OpenRouterOAuth_ExchangeBody(Arena* arena, StrView code, StrView verifier);
OAuthKeyResult OpenRouterOAuth_ParseExchange(Arena* arena, i32 status, StrView body,
                                             StrView transportError);

// POSTs the code to `keysUrl` (<base>/auth/keys) and returns the minted key.
OAuthKeyResult OpenRouterOAuth_Exchange(Arena* arena, StrView keysUrl, StrView code,
                                        StrView verifier, volatile i32* abort);
