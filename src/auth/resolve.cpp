#include "auth/resolve.h"

#include <stdlib.h>

#include "auth/oauth.h"
#include "auth/store.h"

StrView Auth_ResolveToken(Arena* a, StrView authPath, StrView providerId, const char* envVar,
                          volatile i32* abort) {
  if (abort && *abort) return StrView{};
  Credential c;
  if (AuthStore_Read(a, authPath, providerId, &c)) {
    if (c.Kind == CredKind::ApiKey && c.ApiKey.Length) return c.ApiKey;
    if (c.Kind == CredKind::OAuth) {
      if (c.Access.Length && NowMillis() < c.ExpiresMs) return c.Access;
      const OAuthConfig* cfg = OAuth_GetConfig(providerId);
      if (cfg && c.Refresh.Length) {
        Credential nc;
        StrBuf err = StrBuf_Make(a, 64);
        if (OAuth_Refresh(a, cfg, c.Refresh, &nc, &err, abort)) {
          AuthStore_Write(a, authPath, providerId, &nc);
          return nc.Access;
        }
        if (abort && *abort) return StrView{};
      }
      if (c.Access.Length) return c.Access;  // stale; let the provider surface a 401
    }
  }
  if (envVar) {
    const char* e = getenv(envVar);
    if (e && *e) return StrView_Dup(a, SV(e));
  }
  return StrView{nullptr, 0};
}
