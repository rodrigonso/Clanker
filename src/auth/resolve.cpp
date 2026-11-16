#include "auth/resolve.h"

#include <stdlib.h>

#include "auth/store.h"

void AuthTokenCache_Invalidate(AuthTokenCache* cache) {
  if (!cache) return;
  if (cache->Value.Length) Memory_Zero((void*)cache->Value.Data, cache->Value.Length);
  *cache = {};
}

bool AuthTokenCache_Store(Arena* arena, AuthTokenCache* cache, StrView provider,
                          StrView token, StrView accountId, u64 expiresAtMs) {
  if (!arena || !cache || !provider.Length || !token.Length) return false;
  StrView nextProvider = StrView_Dup(arena, provider);
  StrView nextValue = StrView_Dup(arena, token);
  StrView nextAccount = accountId.Length ? StrView_Dup(arena, accountId) : StrView{};
  AuthTokenCache_Invalidate(cache);
  cache->Provider = nextProvider;
  cache->Value = nextValue;
  cache->AccountId = nextAccount;
  cache->ExpiresAtMs = expiresAtMs;
  return true;
}

bool AuthTokenCache_Get(const AuthTokenCache* cache, StrView provider,
                        StrView* out) {
  if (out) *out = {};
  if (!cache || !out || !cache->Value.Length ||
      !StrView_Eq(cache->Provider, provider))
    return false;
  *out = cache->Value;
  return true;
}

StrView Auth_ResolveToken(Arena* a, StrView authPath, StrView providerId,
                          const char* envVar) {
  if (!a) return StrView{};
  Credential c;
  if (AuthStore_Read(a, authPath, providerId, &c) &&
      c.Kind == CredentialKind::ApiKey && c.ApiKey.Length)
    return c.ApiKey;
  if (envVar) {
    const char* e = getenv(envVar);
    if (e && *e) return StrView_Dup(a, SV(e));
  }
  return StrView{};
}
