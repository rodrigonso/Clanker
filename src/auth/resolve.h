#pragma once
#include "auth/credential.h"
#include "base/arena.h"
#include "base/str.h"

// Single-entry memo so repeated turns in a session do not re-read auth.json.
struct AuthTokenCache {
  StrView Provider;
  StrView Value;
};

void AuthTokenCache_Invalidate(AuthTokenCache* cache);
bool AuthTokenCache_Store(Arena* arena, AuthTokenCache* cache, StrView provider,
                          StrView token);
bool AuthTokenCache_Get(const AuthTokenCache* cache, StrView provider,
                        StrView* out);

// Resolves a key for a provider: stored api_key first, then the environment
// variable. Returns an empty view when neither is configured.
StrView Auth_ResolveToken(Arena* arena, StrView authPath, StrView providerId,
                          const char* envVar);
