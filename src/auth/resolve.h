#pragma once
#include "base/arena.h"
#include "base/str.h"

struct ResolvedAuthToken {
  StrView Value;
  u64 ExpiresMs;
};

struct AuthTokenCache {
  StrView Provider;
  StrView Value;
  u64 ExpiresMs;
};

void AuthTokenCache_Invalidate(AuthTokenCache* cache);
bool AuthTokenCache_Store(Arena* arena, AuthTokenCache* cache,
                          StrView provider, const ResolvedAuthToken* token);
bool AuthTokenCache_Get(const AuthTokenCache* cache, StrView provider,
                        u64 nowMs, StrView* out);

// Resolve a usable token for a provider. Priority: stored api_key -> stored OAuth
// (auto-refresh + persist if near expiry) -> environment variable. Optional
// abort applies to refresh transport.
bool Auth_ResolveTokenInfo(Arena* arena, StrView authPath,
                           StrView providerId, const char* envVar,
                           ResolvedAuthToken* out,
                           volatile i32* abort = nullptr);

StrView Auth_ResolveToken(Arena* arena, StrView authPath, StrView providerId, const char* envVar,
                          volatile i32* abort = nullptr);
