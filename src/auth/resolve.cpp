#include "auth/resolve.h"

#include <stdlib.h>

#include "auth/oauth.h"
#include "auth/store.h"
#include "base/thread.h"

void Auth_MergeOAuthRefresh(Arena* a, const Credential* previous,
                            Credential* refreshed) {
  if (!a || !previous || !refreshed) return;
  OAuthChatGptIdentity identity = {};
  bool haveNewIdentity =
      (refreshed->IdToken.Length &&
       OAuth_ParseChatGptIdentity(a, refreshed->IdToken, &identity)) ||
      (refreshed->Access.Length &&
       OAuth_ParseChatGptIdentity(a, refreshed->Access, &identity));
  if (haveNewIdentity) {
    refreshed->AccountId = identity.AccountId;
    refreshed->IsFedRamp = identity.IsFedRamp;
  } else {
    refreshed->IsFedRamp = previous->IsFedRamp;
  }
  if (!refreshed->AccountId.Length)
    refreshed->AccountId = previous->AccountId;
  if (!refreshed->AccountId.Length) {
    OAuthChatGptIdentity previousIdentity = {};
    if ((previous->IdToken.Length &&
         OAuth_ParseChatGptIdentity(a, previous->IdToken,
                                    &previousIdentity)) ||
        (previous->Access.Length &&
         OAuth_ParseChatGptIdentity(a, previous->Access,
                                    &previousIdentity))) {
      refreshed->AccountId = previousIdentity.AccountId;
      refreshed->IsFedRamp = previousIdentity.IsFedRamp;
    }
  }
  if (!refreshed->Access.Length) refreshed->Access = previous->Access;
  if (!refreshed->Refresh.Length) refreshed->Refresh = previous->Refresh;
  if (!refreshed->IdToken.Length) refreshed->IdToken = previous->IdToken;
  if (!refreshed->ExpiresMs) refreshed->ExpiresMs = previous->ExpiresMs;
}

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
  cache->Kind = token->Kind;
  cache->AccountId = StrView_Dup(arena, token->AccountId);
  cache->IsFedRamp = token->IsFedRamp;
  return true;
}

bool AuthTokenCache_Get(const AuthTokenCache* cache, StrView provider,
                        u64 nowMs, ResolvedAuthToken* out) {
  if (out) *out = {};
  if (!cache || !out || !cache->Value.Length ||
      !StrView_Eq(cache->Provider, provider) ||
      (cache->ExpiresMs && nowMs >= cache->ExpiresMs))
    return false;
  out->Value = cache->Value;
  out->ExpiresMs = cache->ExpiresMs;
  out->Kind = cache->Kind;
  out->AccountId = cache->AccountId;
  out->IsFedRamp = cache->IsFedRamp;
  return true;
}

bool Auth_ResolveTokenInfo(Arena* a, StrView authPath, StrView providerId,
                           const char* envVar, ResolvedAuthToken* out,
                           volatile i32* abort) {
  if (!a || !out || (abort && Atomic_LoadI32(abort))) return false;
  *out = {};
  Credential c;
  if (AuthStore_Read(a, authPath, providerId, &c)) {
    if (c.Kind == CredKind::ApiKey && c.ApiKey.Length) {
      out->Value = c.ApiKey;
      out->Kind = CredKind::ApiKey;
      return true;
    }
    if (c.Kind == CredKind::OAuth) {
      StrView accountId = c.AccountId;
      bool isFedRamp = c.IsFedRamp;
      OAuthChatGptIdentity identity = {};
      if ((c.IdToken.Length &&
           OAuth_ParseChatGptIdentity(a, c.IdToken, &identity)) ||
          (c.Access.Length &&
           OAuth_ParseChatGptIdentity(a, c.Access, &identity))) {
        if (!accountId.Length) accountId = identity.AccountId;
        isFedRamp = identity.IsFedRamp;
      }
      if (c.Access.Length && NowMillis() < c.ExpiresMs) {
        out->Value = c.Access;
        out->ExpiresMs = c.ExpiresMs;
        out->Kind = CredKind::OAuth;
        out->AccountId = accountId;
        out->IsFedRamp = isFedRamp;
        return true;
      }
      const OAuthConfig* cfg = OAuth_GetConfig(providerId);
      if (cfg && c.Refresh.Length) {
        Credential nc;
        StrBuf err = StrBuf_Make(a, 64);
        if (OAuth_Refresh(a, cfg, c.Refresh, &nc, &err, abort)) {
          Auth_MergeOAuthRefresh(a, &c, &nc);
          AuthStore_Write(a, authPath, providerId, &nc);
          out->Value = nc.Access;
          out->ExpiresMs = nc.ExpiresMs;
          out->Kind = CredKind::OAuth;
          out->AccountId = nc.AccountId;
          out->IsFedRamp = nc.IsFedRamp;
          return out->Value.Length > 0;
        }
        if (abort && Atomic_LoadI32(abort)) return false;
      }
      if (c.Access.Length) {
        out->Value = c.Access;
        out->ExpiresMs = c.ExpiresMs ? c.ExpiresMs : NowMillis();
        out->Kind = CredKind::OAuth;
        out->AccountId = accountId;
        out->IsFedRamp = isFedRamp;
        return true;  // stale; let the provider surface a 401
      }
    }
  }
  if (envVar) {
    const char* e = getenv(envVar);
    if (e && *e) {
      out->Value = StrView_Dup(a, SV(e));
      out->Kind = CredKind::ApiKey;
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
