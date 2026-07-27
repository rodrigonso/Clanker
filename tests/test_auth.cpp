#include <stdio.h>

#include "auth/oauth.h"
#include "auth/resolve.h"
#include "auth/store.h"
#include "base/arena.h"
#include "base/str.h"
#include "check.h"

static void TestStore() {
  Arena a = ArenaCreate();
  StrView path = SVL("clank_auth_test.json");
  remove("clank_auth_test.json");

  Credential ak = {};
  ak.Kind = CredKind::ApiKey;
  ak.ApiKey = SVL("sk-test-123");
  CHECK(AuthStore_Write(&a, path, SVL("openai"), &ak));

  Credential r = {};
  CHECK(AuthStore_Read(&a, path, SVL("openai"), &r));
  CHECK_ALL(r.Kind == CredKind::ApiKey, StrView_EqCStr(r.ApiKey, "sk-test-123"));

  Credential oc = {};
  oc.Kind = CredKind::OAuth;
  oc.Access = SVL("acc");
  oc.Refresh = SVL("ref");
  oc.ExpiresMs = 1234567890123ull;
  CHECK(AuthStore_Write(&a, path, SVL("anthropic"), &oc));

  Credential r2 = {};
  CHECK(AuthStore_Read(&a, path, SVL("anthropic"), &r2));
  CHECK_ALL(r2.Kind == CredKind::OAuth, StrView_EqCStr(r2.Access, "acc"),
            StrView_EqCStr(r2.Refresh, "ref"),
            r2.ExpiresMs == 1234567890123ull);
  volatile i32 abort = 1;
  CHECK(Auth_ResolveToken(&a, path, SVL("anthropic"), nullptr, &abort).Length == 0);

  // Both providers coexist.
  Credential chk = {};
  CHECK(AuthStore_Read(&a, path, SVL("openai"), &chk));
  CHECK(StrView_EqCStr(chk.ApiKey, "sk-test-123"));

  // Update in place.
  oc.Access = SVL("acc2");
  CHECK(AuthStore_Write(&a, path, SVL("anthropic"), &oc));
  CHECK(AuthStore_Read(&a, path, SVL("anthropic"), &r2));
  CHECK(StrView_EqCStr(r2.Access, "acc2"));

  // Delete one, keep the other.
  CHECK(AuthStore_Delete(&a, path, SVL("openai")));
  Credential gone = {};
  CHECK(!AuthStore_Read(&a, path, SVL("openai"), &gone));
  CHECK(AuthStore_Read(&a, path, SVL("anthropic"), &r2));

  remove("clank_auth_test.json");
  ArenaRelease(&a);
}

static void TestOAuth() {
  Arena a = ArenaCreate();

  const OAuthConfig* cfg = OAuth_GetConfig(SVL("anthropic"));
  CHECK_ALL(cfg != nullptr, OAuth_GetConfig(SVL("nope")) == nullptr);

  OAuthBegin beg = OAuth_BeginAuthCode(&a, cfg);
  CHECK_ALL(beg.Verifier.Length == 43,
            StrView_Contains(beg.AuthUrl, "https://claude.ai/oauth/authorize?"),
            StrView_Contains(beg.AuthUrl, "client_id=9d1c250a"),
            StrView_Contains(beg.AuthUrl, "code_challenge_method=S256"),
            StrView_Contains(beg.AuthUrl,
                             "scope=org%3Acreate_api_key"));  // ':' url-encoded

  Credential tr = {};
  CHECK(OAuth_ParseTokenResponse(
      &a, SV("{\"access_token\":\"at\",\"refresh_token\":\"rt\",\"expires_in\":3600}"), &tr));
  CHECK_ALL(tr.Kind == CredKind::OAuth,
            StrView_EqCStr(tr.Access, "at") && StrView_EqCStr(tr.Refresh, "rt"),
            tr.ExpiresMs > NowMillis());

  volatile i32 abort = 1;
  Credential cancelled = {};
  StrBuf error = StrBuf_Make(&a, 64);
  CHECK(!OAuth_ExchangeCode(&a, cfg, SVL("code"), SVL("state"), SVL("verifier"),
                            &cancelled, &error, &abort));
  CHECK(StrView_Contains(StrBuf_View(&error), "cancelled"));
  StrBuf_Clear(&error);
  CHECK(!OAuth_Refresh(&a, cfg, SVL("refresh"), &cancelled, &error, &abort));
  CHECK(StrView_Contains(StrBuf_View(&error), "cancelled"));

  ArenaRelease(&a);
}

void TestAuth() {
  TestStore();
  TestOAuth();
}
