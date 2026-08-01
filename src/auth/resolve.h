#pragma once
#include "auth/credential.h"
#include "base/arena.h"
#include "base/str.h"

// Kind is ApiKey for plain keys and env-var fallbacks; OAuth for any credential
// that came from the stored OAuth access token, whether fresh, just-refreshed,
// or stale (refresh failed/unavailable).
struct ResolvedAuthToken {
  StrView Value;
  u64 ExpiresMs;
  CredKind Kind;
  StrView AccountId;
  bool IsFedRamp;
};

struct AuthTokenCache {
  StrView Provider;
  StrView Value;
  u64 ExpiresMs;
  CredKind Kind;
  StrView AccountId;
  bool IsFedRamp;
};

// Carries forward fields that OAuth refresh responses may omit and derives
// provider account metadata from any newly issued identity/access token.
void Auth_MergeOAuthRefresh(Arena* arena, const Credential* previous,
                            Credential* refreshed);

void AuthTokenCache_Invalidate(AuthTokenCache* cache);
bool AuthTokenCache_Store(Arena* arena, AuthTokenCache* cache,
                          StrView provider, const ResolvedAuthToken* token);
bool AuthTokenCache_Get(const AuthTokenCache* cache, StrView provider,
                        u64 nowMs, ResolvedAuthToken* out);

// Resolve a usable token for a provider. Priority: stored api_key -> stored OAuth
// (auto-refresh + persist if near expiry) -> environment variable. Optional
// abort applies to refresh transport.
bool Auth_ResolveTokenInfo(Arena* arena, StrView authPath,
                           StrView providerId, const char* envVar,
                           ResolvedAuthToken* out,
                           volatile i32* abort = nullptr);

StrView Auth_ResolveToken(Arena* arena, StrView authPath, StrView providerId, const char* envVar,
                          volatile i32* abort = nullptr);
