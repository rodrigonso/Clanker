#pragma once
#include "base/basics.h"
#include "base/str.h"

// One stored credential per provider. An API key is a bearer token that does
// not expire; OpenRouter's `/auth` mints one through OAuth PKCE. An OAuth
// credential is a short-lived access token plus the refresh token that renews
// it, which is what ChatGPT sign-in issues for the Codex backend.
enum class CredentialKind : u8 { ApiKey, OAuth };

struct Credential {
  CredentialKind Kind;
  StrView ApiKey;
  StrView AccessToken;
  StrView RefreshToken;
  StrView AccountId;  // provider account the token is scoped to, if any
  u64 ExpiresAtMs;    // Unix epoch milliseconds
};

// Access tokens are renewed this long before they lapse so a request that is
// already in flight cannot outlive the token it carries.
constexpr u64 CREDENTIAL_REFRESH_MARGIN_MS = 5 * 60 * 1000;

inline bool Credential_NeedsRefresh(u64 expiresAtMs, u64 nowMs) {
  return expiresAtMs <= nowMs + CREDENTIAL_REFRESH_MARGIN_MS;
}
