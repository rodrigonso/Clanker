#include <stdio.h>

#include "auth/oauth.h"
#include "auth/resolve.h"
#include "auth/store.h"
#include "base/arena.h"
#include "base/str.h"
#include "check.h"
#include "crypto/base64.h"
#include "json/json.h"

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
  ResolvedAuthToken apiToken = {};
  CHECK(Auth_ResolveTokenInfo(&a, path, SVL("openai"), nullptr, &apiToken));
  CHECK_ALL(StrView_EqCStr(apiToken.Value, "sk-test-123"),
            apiToken.ExpiresMs == 0,
            apiToken.Kind == CredKind::ApiKey);

  Credential oc = {};
  oc.Kind = CredKind::OAuth;
  oc.Access = SVL("acc");
  oc.Refresh = SVL("ref");
  oc.ExpiresMs = NowMillis() + 60000;
  oc.IdToken = SVL("identity");
  oc.AccountId = SVL("account");
  oc.IsFedRamp = true;
  CHECK(AuthStore_Write(&a, path, SVL("anthropic"), &oc));

  Credential r2 = {};
  CHECK(AuthStore_Read(&a, path, SVL("anthropic"), &r2));
  CHECK_ALL(r2.Kind == CredKind::OAuth, StrView_EqCStr(r2.Access, "acc"),
            StrView_EqCStr(r2.Refresh, "ref"),
            r2.ExpiresMs == oc.ExpiresMs,
            StrView_EqCStr(r2.IdToken, "identity"),
            StrView_EqCStr(r2.AccountId, "account"),
            r2.IsFedRamp);
  ResolvedAuthToken oauthToken = {};
  CHECK(Auth_ResolveTokenInfo(&a, path, SVL("anthropic"), nullptr,
                              &oauthToken));
  CHECK_ALL(StrView_EqCStr(oauthToken.Value, "acc"),
            oauthToken.ExpiresMs == oc.ExpiresMs,
            oauthToken.Kind == CredKind::OAuth,
            StrView_EqCStr(oauthToken.AccountId, "account"),
            oauthToken.IsFedRamp);
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

  Credential stale = {};
  stale.Kind = CredKind::OAuth;
  stale.Access = SVL("stale");
  CHECK(AuthStore_Write(&a, path, SVL("anthropic"), &stale));
  ResolvedAuthToken staleToken = {};
  CHECK(Auth_ResolveTokenInfo(&a, path, SVL("anthropic"), nullptr,
                              &staleToken));
  CHECK_ALL(StrView_EqCStr(staleToken.Value, "stale"),
            staleToken.ExpiresMs > 0,
            staleToken.ExpiresMs <= NowMillis(),
            staleToken.Kind == CredKind::OAuth);

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
            beg.State.Length == 43,
            !StrView_Eq(beg.Verifier, beg.State),  // independent randomness, not reused
            StrView_Contains(beg.AuthUrl, "https://claude.ai/oauth/authorize?"),
            StrView_Contains(beg.AuthUrl, "code=true&client_id=9d1c250a"),
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

  // OpenAI: config-driven authorize extras, form-urlencoded token requests.
  const OAuthConfig* openai = OAuth_GetConfig(SVL("openai"));
  CHECK(openai != nullptr);
  OAuthBegin obeg = OAuth_BeginAuthCode(&a, openai);
  CHECK_ALL(obeg.Verifier.Length == 43,
            obeg.State.Length == 43,
            !StrView_Eq(obeg.Verifier, obeg.State),
            StrView_Contains(obeg.AuthUrl, "https://auth.openai.com/oauth/authorize?"),
            StrView_Contains(obeg.AuthUrl, "id_token_add_organizations=true"),
            StrView_Contains(obeg.AuthUrl, "codex_cli_simplified_flow=true"),
            StrView_Contains(obeg.AuthUrl, "originator=clank"),
            StrView_Contains(obeg.AuthUrl, "client_id=app_EMoamEEZ73f0CkXaXp7hrann"),
            StrView_Contains(obeg.AuthUrl, "scope=openid"),
            StrView_Contains(obeg.AuthUrl, "api.connectors.invoke"));

  StrBuf_Clear(&error);
  CHECK(!OAuth_ExchangeCode(&a, openai, SVL("code"), SVL("state"), SVL("verifier"),
                            &cancelled, &error, &abort));
  CHECK(StrView_Contains(StrBuf_View(&error), "cancelled"));
  StrBuf_Clear(&error);
  CHECK(!OAuth_Refresh(&a, openai, SVL("refresh"), &cancelled, &error, &abort));
  CHECK(StrView_Contains(StrBuf_View(&error), "cancelled"));

  ArenaRelease(&a);
}

static void TestChatGptAccountIdClaim() {
  Arena a = ArenaCreate();

  auto makeJwt = [&](StrView payloadJson) {
    StrView payloadB64 = Base64UrlEncode(&a, (const byte*)payloadJson.Data, payloadJson.Length);
    StrBuf jwt = StrBuf_Make(&a, 256);
    StrBuf_AppendCStr(&jwt, "header.");
    StrBuf_Append(&jwt, payloadB64);
    StrBuf_AppendCStr(&jwt, ".signature");
    return StrView_Dup(&a, StrBuf_View(&jwt));
  };

  StrView out = {};
  StrView goodJwt = makeJwt(
      SVL("{\"https://api.openai.com/auth\":{\"chatgpt_account_id\":"
          "\"acct_123\",\"chatgpt_account_is_fedramp\":true}}"));
  CHECK(OAuth_ParseChatGptAccountId(&a, goodJwt, &out));
  CHECK(StrView_EqCStr(out, "acct_123"));
  OAuthChatGptIdentity identity = {};
  CHECK(OAuth_ParseChatGptIdentity(&a, goodJwt, &identity));
  CHECK_ALL(StrView_EqCStr(identity.AccountId, "acct_123"),
            identity.IsFedRamp);

  StrBuf tokenJson = StrBuf_Make(&a, goodJwt.Length + 96);
  StrBuf_AppendCStr(&tokenJson, "{\"access_token\":\"opaque-access\","
                                 "\"refresh_token\":\"refresh\","
                                 "\"expires_in\":3600,\"id_token\":");
  Json_WriteString(&tokenJson, goodJwt);
  StrBuf_AppendChar(&tokenJson, '}');
  Credential parsed = {};
  CHECK(OAuth_ParseTokenResponse(&a, StrBuf_View(&tokenJson), &parsed));
  CHECK_ALL(StrView_Eq(parsed.IdToken, goodJwt),
            StrView_EqCStr(parsed.AccountId, "acct_123"),
            parsed.IsFedRamp);

  Credential partial = {};
  CHECK(OAuth_ParseRefreshTokenResponse(
      &a, SVL("{\"refresh_token\":\"rotated-refresh\"}"), &partial));
  CHECK_ALL(!partial.Access.Length,
            StrView_EqCStr(partial.Refresh, "rotated-refresh"),
            !partial.IdToken.Length, partial.ExpiresMs == 0);
  CHECK(!OAuth_ParseRefreshTokenResponse(&a, SVL("{}"), &partial));

  Credential previous = {};
  previous.Kind = CredKind::OAuth;
  previous.Access = SVL("previous-access");
  previous.Refresh = SVL("previous-refresh");
  previous.IdToken = goodJwt;
  previous.AccountId = SVL("acct_123");
  previous.IsFedRamp = true;
  previous.ExpiresMs = 1234;
  Credential refreshed = {};
  refreshed.Kind = CredKind::OAuth;
  refreshed.Refresh = SVL("rotated-refresh");
  Auth_MergeOAuthRefresh(&a, &previous, &refreshed);
  CHECK_ALL(StrView_EqCStr(refreshed.Access, "previous-access"),
            StrView_EqCStr(refreshed.Refresh, "rotated-refresh"),
            StrView_Eq(refreshed.IdToken, goodJwt),
            StrView_EqCStr(refreshed.AccountId, "acct_123"),
            refreshed.IsFedRamp,
            refreshed.ExpiresMs == 1234);

  // Malformed: not a 3-segment JWT.
  out = {};
  CHECK(!OAuth_ParseChatGptAccountId(&a, SVL("not-a-jwt"), &out));
  CHECK(out.Length == 0);

  // Malformed: empty header segment.
  CHECK(!OAuth_ParseChatGptAccountId(&a, SVL(".payload.sig"), &out));

  // Malformed: empty signature segment.
  StrView payloadB64 =
      Base64UrlEncode(&a, (const byte*)"{}", 2);
  StrBuf noSig = StrBuf_Make(&a, 64);
  StrBuf_AppendCStr(&noSig, "header.");
  StrBuf_Append(&noSig, payloadB64);
  StrBuf_AppendCStr(&noSig, ".");
  CHECK(!OAuth_ParseChatGptAccountId(&a, StrBuf_View(&noSig), &out));

  // Malformed: invalid base64url payload.
  CHECK(!OAuth_ParseChatGptAccountId(&a, SVL("header.not!valid!base64.sig"), &out));

  // Malformed: valid base64url but not JSON.
  StrView notJsonB64 = Base64UrlEncode(&a, (const byte*)"not json", 8);
  StrBuf notJsonJwt = StrBuf_Make(&a, 64);
  StrBuf_AppendCStr(&notJsonJwt, "header.");
  StrBuf_Append(&notJsonJwt, notJsonB64);
  StrBuf_AppendCStr(&notJsonJwt, ".sig");
  CHECK(!OAuth_ParseChatGptAccountId(&a, StrBuf_View(&notJsonJwt), &out));

  // Malformed: valid JSON but missing the auth claim entirely.
  CHECK(!OAuth_ParseChatGptAccountId(&a, makeJwt(SVL("{}")), &out));

  // Malformed: auth claim present but missing chatgpt_account_id field.
  CHECK(!OAuth_ParseChatGptAccountId(
      &a, makeJwt(SVL("{\"https://api.openai.com/auth\":{\"chatgpt_user_id\":\"u1\"}}")), &out));

  ArenaRelease(&a);
}

static void TestTokenCache() {
  Arena a = ArenaCreate();
  AuthTokenCache cache = {};
  char source[] = "cached-token";
  ResolvedAuthToken token = {SV(source), 2000, CredKind::OAuth,
                             SVL("account"), true};
  CHECK(AuthTokenCache_Store(&a, &cache, SVL("anthropic"), &token));
  source[0] = 'X';

  ResolvedAuthToken resolved = {};
  CHECK(AuthTokenCache_Get(&cache, SVL("anthropic"), 1999, &resolved));
  CHECK_ALL(StrView_EqCStr(resolved.Value, "cached-token"),
            resolved.Kind == CredKind::OAuth,
            StrView_EqCStr(resolved.AccountId, "account"),
            resolved.IsFedRamp);
  CHECK(!AuthTokenCache_Get(&cache, SVL("openai"), 1999, &resolved));
  CHECK(!AuthTokenCache_Get(&cache, SVL("anthropic"), 2000, &resolved));

  ResolvedAuthToken apiKey = {SVL("api-key"), 0, CredKind::ApiKey};
  CHECK(AuthTokenCache_Store(&a, &cache, SVL("openai"), &apiKey));
  CHECK(AuthTokenCache_Get(&cache, SVL("openai"), 999999, &resolved));
  CHECK_ALL(StrView_EqCStr(resolved.Value, "api-key"), resolved.Kind == CredKind::ApiKey);

  AuthTokenCache_Invalidate(&cache);
  CHECK(!AuthTokenCache_Get(&cache, SVL("openai"), 0, &resolved));
  ArenaRelease(&a);
}

void TestAuth() {
  TestStore();
  TestOAuth();
  TestChatGptAccountIdClaim();
  TestTokenCache();
}
