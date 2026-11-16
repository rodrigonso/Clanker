#pragma once
#include "auth/credential.h"
#include "base/arena.h"
#include "base/str.h"

// Single-entry memo so repeated turns in a session do not re-read auth.json.
struct AuthTokenCache {
  StrView Provider;
  StrView Value;
  StrView AccountId;
  u64 ExpiresAtMs;  // zero for a key that does not expire
};

void AuthTokenCache_Invalidate(AuthTokenCache* cache);
bool AuthTokenCache_Store(Arena* arena, AuthTokenCache* cache, StrView provider,
                          StrView token, StrView accountId = {},
                          u64 expiresAtMs = 0);
bool AuthTokenCache_Get(const AuthTokenCache* cache, StrView provider,
                        StrView* out);

// Resolves a key for a provider: stored api_key first, then the environment
// variable. Returns an empty view when neither is configured. OAuth entries
// are not keys; their tokens expire and are resolved by the sign-in flow.
StrView Auth_ResolveToken(Arena* arena, StrView authPath, StrView providerId,
                          const char* envVar);
