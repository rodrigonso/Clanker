#include "auth/resolve.h"

#include <stdlib.h>

#include "auth/oauth.h"
#include "auth/store.h"

void AuthTokenCache_Invalidate(AuthTokenCache* cache) {
  if (cache) *cache = {};
}

bool AuthTokenCache_Store(Arena* arena, AuthTokenCache* cache,
                          StrView provider, const ResolvedAuthToken* token) {
  if (!arena || !cache || !provider.Length || !token ||
      !token->Value.Length)
    return false;
  cache->Provider = StrView_Dup(arena, provider);
  cache->Value = StrView_Dup(arena, token->Value);
  cache->ExpiresMs = token->ExpiresMs;
  return true;
}

bool AuthTokenCache_Get(const AuthTokenCache* cache, StrView provider,
                        u64 nowMs, StrView* out) {
  if (out) *out = {};
  if (!cache || !out || !cache->Value.Length ||
      !StrView_Eq(cache->Provider, provider) ||
      (cache->ExpiresMs && nowMs >= cache->ExpiresMs))
    return false;
  *out = cache->Value;
  return true;
}

bool Auth_ResolveTokenInfo(Arena* a, StrView authPath, StrView providerId,
                           const char* envVar, ResolvedAuthToken* out,
                           volatile i32* abort) {
  if (!a || !out || (abort && *abort)) return false;
  *out = {};
  Credential c;
  if (AuthStore_Read(a, authPath, providerId, &c)) {
    if (c.Kind == CredKind::ApiKey && c.ApiKey.Length) {
      out->Value = c.ApiKey;
      return true;
    }
    if (c.Kind == CredKind::OAuth) {
      if (c.Access.Length && NowMillis() < c.ExpiresMs) {
        out->Value = c.Access;
        out->ExpiresMs = c.ExpiresMs;
        return true;
      }
      const OAuthConfig* cfg = OAuth_GetConfig(providerId);
      if (cfg && c.Refresh.Length) {
        Credential nc;
        StrBuf err = StrBuf_Make(a, 64);
        if (OAuth_Refresh(a, cfg, c.Refresh, &nc, &err, abort)) {
          AuthStore_Write(a, authPath, providerId, &nc);
          out->Value = nc.Access;
          out->ExpiresMs = nc.ExpiresMs;
          return out->Value.Length > 0;
        }
        if (abort && *abort) return false;
      }
      if (c.Access.Length) {
        out->Value = c.Access;
        out->ExpiresMs = c.ExpiresMs ? c.ExpiresMs : NowMillis();
        return true;  // stale; let the provider surface a 401
      }
    }
  }
  if (envVar) {
    const char* e = getenv(envVar);
    if (e && *e) {
      out->Value = StrView_Dup(a, SV(e));
      return true;
    }
  }
  return false;
}

StrView Auth_ResolveToken(Arena* a, StrView authPath, StrView providerId,
                          const char* envVar, volatile i32* abort) {
  ResolvedAuthToken resolved = {};
  return Auth_ResolveTokenInfo(a, authPath, providerId, envVar, &resolved,
                               abort)
             ? resolved.Value
             : StrView{};
}
