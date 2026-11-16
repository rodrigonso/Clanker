#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/openai_codex_oauth.h"
#include "auth/openrouter_oauth.h"
#include "auth/resolve.h"
#include "auth/store.h"
#include "base/arena.h"
#include "base/config_file.h"
#include "base/str.h"
#include "base/thread.h"
#include "check.h"
#include "crypto/base64.h"
#include "net/http.h"
#include "net/loopback.h"

static void SetTestEnv(const char* name, const char* value) {
#if defined(_WIN32)
  _putenv_s(name, value ? value : "");
#else
  if (value)
    setenv(name, value, 1);
  else
    unsetenv(name);
#endif
}

static void TestStore() {
  Arena a = ArenaCreate();
  StrView path = SVL("clanker_auth_test.json");
  remove("clanker_auth_test.json");

  Credential key = {};
  key.ApiKey = SVL("sk-test-123");
  CHECK(AuthStore_Write(&a, path, SVL("provider-a"), &key));

  Credential read = {};
  CHECK(AuthStore_Read(&a, path, SVL("provider-a"), &read));
  CHECK(StrView_EqCStr(read.ApiKey, "sk-test-123"));
  CHECK(StrView_EqCStr(Auth_ResolveToken(&a, path, SVL("provider-a"), nullptr),
                       "sk-test-123"));

  // A second provider does not disturb the first, and an unknown provider
  // resolves to nothing rather than to some other provider's key.
  Credential other = {};
  other.ApiKey = SVL("sk-other");
  CHECK(AuthStore_Write(&a, path, SVL("anthropic"), &other));
  CHECK(AuthStore_Read(&a, path, SVL("provider-a"), &read));
  CHECK(StrView_EqCStr(read.ApiKey, "sk-test-123"));
  CHECK(Auth_ResolveToken(&a, path, SVL("nope"), nullptr).Length == 0);

  // Writing an existing provider replaces rather than appends.
  Credential replacement = {};
  replacement.ApiKey = SVL("sk-replaced");
  CHECK(AuthStore_Write(&a, path, SVL("provider-a"), &replacement));
  CHECK(AuthStore_Read(&a, path, SVL("provider-a"), &read));
  CHECK(StrView_EqCStr(read.ApiKey, "sk-replaced"));

  CHECK(AuthStore_Delete(&a, path, SVL("provider-a")));
  Credential gone = {};
  CHECK(!AuthStore_Read(&a, path, SVL("provider-a"), &gone));
  CHECK(AuthStore_Read(&a, path, SVL("anthropic"), &read));
  CHECK(StrView_EqCStr(read.ApiKey, "sk-other"));

  // An oauth entry without its refresh token and expiry is not a credential,
  // and no malformed entry is ever read as a key.
  CHECK(ConfigFile_WriteAtomic(
      &a, path, SVL("{\"openai\":{\"type\":\"oauth\",\"access\":\"tok\"}}")));
  CHECK(!AuthStore_Read(&a, path, SVL("openai"), &gone));
  CHECK(Auth_ResolveToken(&a, path, SVL("openai"), nullptr).Length == 0);

  remove("clanker_auth_test.json");
  ArenaRelease(&a);
}

static void TestOAuthStore() {
  Arena a = ArenaCreate();
  StrView path = SVL("clanker_auth_oauth_test.json");
  remove("clanker_auth_oauth_test.json");

  Credential signedIn = {};
  signedIn.Kind = CredentialKind::OAuth;
  signedIn.AccessToken = SVL("acc\"ess");
  signedIn.RefreshToken = SVL("refresh-1");
  signedIn.AccountId = SVL("acct-1");
  signedIn.ExpiresAtMs = 1700000000123;
  CHECK(AuthStore_Write(&a, path, SVL("openai-codex"), &signedIn));
  StrView content = {};
  CHECK(ConfigFile_ReadAll(&a, path, &content) == ConfigFileReadResult::Success);
  CHECK(StrView_EqCStr(content,
                       "{\"openai-codex\":{\"type\":\"oauth\",\"access\":\"acc\\\"ess\","
                       "\"refresh\":\"refresh-1\",\"expires\":1700000000123,"
                       "\"accountId\":\"acct-1\"}}"));

  Credential read = {};
  CHECK(AuthStore_Read(&a, path, SVL("openai-codex"), &read));
  CHECK_ALL(read.Kind == CredentialKind::OAuth,
            StrView_EqCStr(read.AccessToken, "acc\"ess"),
            StrView_EqCStr(read.RefreshToken, "refresh-1"),
            StrView_EqCStr(read.AccountId, "acct-1"),
            read.ExpiresAtMs == 1700000000123, read.ApiKey.Length == 0);
  // A short-lived token is never handed out as a static key.
  CHECK(Auth_ResolveToken(&a, path, SVL("openai-codex"), nullptr).Length == 0);

  // Rewriting the store for another provider keeps the sign-in intact, and an
  // entry without an account id round-trips without one.
  Credential key = {};
  key.ApiKey = SVL("sk-key");
  CHECK(AuthStore_Write(&a, path, SVL("openrouter"), &key));
  Credential noAccount = signedIn;
  noAccount.AccountId = {};
  noAccount.ExpiresAtMs = 9007199254740992ull;
  CHECK(AuthStore_Write(&a, path, SVL("openai-codex"), &noAccount));
  CHECK(AuthStore_Read(&a, path, SVL("openai-codex"), &read));
  CHECK_ALL(read.AccountId.Length == 0, read.ExpiresAtMs == 9007199254740992ull);
  CHECK(AuthStore_Read(&a, path, SVL("openrouter"), &read));
  CHECK(StrView_EqCStr(read.ApiKey, "sk-key"));

  // Nothing is written that could not be read back exactly.
  Credential unwritable = signedIn;
  unwritable.ExpiresAtMs = 9007199254740993ull;
  CHECK(!AuthStore_Write(&a, path, SVL("openai-codex"), &unwritable));
  unwritable = signedIn;
  unwritable.AccessToken = {};
  CHECK(!AuthStore_Write(&a, path, SVL("openai-codex"), &unwritable));
  unwritable = signedIn;
  unwritable.RefreshToken = {};
  CHECK(!AuthStore_Write(&a, path, SVL("openai-codex"), &unwritable));
  CHECK(AuthStore_Read(&a, path, SVL("openai-codex"), &read));
  CHECK(read.ExpiresAtMs == 9007199254740992ull);

  remove("clanker_auth_oauth_test.json");
  ArenaReleaseSecure(&a);
}

static void TestStoreFailurePreservesContent() {
  Arena arena = ArenaCreate();
  StrView path = SVL("clanker_auth_failure_test.json");
  Credential replacement = {};
  replacement.ApiKey = SVL("synthetic-replacement");
  const StrView invalidStores[] = {
      SVL(""), SVL("{"), SVL("{ \r\n\t"), SVL("[]"), SVL("null"),
      SVL("{\"other\":{"),
      SVL("{\"other\":{\"type\":\"api_key\",\"key\":\"synthetic-old\"}},"),
      SVL("{\"other\":{\"type\":\"api_key\",\"key\":\"synthetic-old\"},"),
      SVL("{\"other\":{\"type\":\"api_key\",\"key\":\"synthetic\nold\"}}"),
      SVL("{\"other\":{\"type\":\"api_key\",\"key\":\"synthetic\\qold\"}}"),
      SVL("{\"other\":{\"type\":\"api_key\",\"key\":42}}"),
      SVL("{\"other\":{\"type\":\"api_key\",\"key\":\"a\",\"key\":\"b\"}}"),
      SVL("{\"other\":{\"type\":\"api_key\",\"key\":\"a\",\"extra\":\"preserve\"}}"),
      SVL("{\"other\":{\"type\":\"unsupported\",\"key\":\"synthetic-old\"}}"),
      SVL("{\"other\":{\"type\":\"api_key\",\"key\":\"a\"},\"other\":{\"type\":\"api_key\",\"key\":\"b\"}}"),
      SVL("{\"other\":{\"type\":\"oauth\",\"access\":\"a\",\"refresh\":\"r\",\"expires\":1,\"extra\":true}}"),
      SVL("{\"other\":{\"type\":\"oauth\",\"access\":\"a\",\"refresh\":\"r\",\"expires\":1.5}}"),
      SVL("{\"other\":{\"type\":\"oauth\",\"access\":\"a\",\"refresh\":\"r\",\"expires\":-1}}"),
      SVL("{\"other\":{\"type\":\"oauth\",\"access\":\"a\",\"refresh\":\"r\",\"expires\":\"1\"}}"),
      SVL("{\"other\":{\"type\":\"oauth\",\"access\":\"a\",\"refresh\":\"r\",\"expires\":1e300}}"),
      SVL("{\"other\":{\"type\":\"oauth\",\"access\":\"a\",\"expires\":1}}"),
      SVL("{\"other\":{\"type\":\"oauth\",\"access\":\"\",\"refresh\":\"r\",\"expires\":1}}"),
      SVL("{\"other\":{\"type\":\"oauth\",\"access\":\"a\",\"refresh\":\"r\",\"expires\":1,\"accountId\":7}}"),
      SVL("{\"typesafe\":{\"type\":\"api_key\",\"key\":\"synthetic-old\"},\"other\":false}")};
  for (StrView content : invalidStores) {
    CHECK(ConfigFile_WriteAtomic(&arena, path, content));
    CHECK(!AuthStore_Write(&arena, path, SVL("typesafe"), &replacement));
    CHECK(!AuthStore_Delete(&arena, path, SVL("typesafe")));
    StrView after = {};
    CHECK(ConfigFile_ReadAll(&arena, path, &after) == ConfigFileReadResult::Success);
    CHECK(StrView_Eq(after, content));
  }
  Credential usable = {};
  CHECK(AuthStore_Read(&arena, path, SVL("typesafe"), &usable));
  CHECK(StrView_EqCStr(usable.ApiKey, "synthetic-old"));
  StrView complete = SVL("{\"typesafe\":{\"type\":\"api_key\",\"key\":\"synthetic-old\"}}");
  for (u32 length = 0; length < complete.Length; length++) {
    StrView truncated = {complete.Data, length};
    CHECK(ConfigFile_WriteAtomic(&arena, path, truncated));
    CHECK(!AuthStore_Write(&arena, path, SVL("typesafe"), &replacement));
    CHECK(!AuthStore_Delete(&arena, path, SVL("typesafe")));
    CHECK(!AuthStore_Read(&arena, path, SVL("typesafe"), &usable));
    StrView after = {};
    CHECK(ConfigFile_ReadAll(&arena, path, &after) == ConfigFileReadResult::Success);
    CHECK(StrView_Eq(after, truncated));
  }
  remove("clanker_auth_failure_test.json");
  CHECK(AuthStore_Write(&arena, path, SVL("typesafe"), &replacement));
  CHECK(AuthStore_Read(&arena, path, SVL("typesafe"), &usable));
  CHECK(StrView_Eq(usable.ApiKey, replacement.ApiKey));
  remove("clanker_auth_failure_test.json");
  ArenaReleaseSecure(&arena);
}

static void TestResolve() {
  Arena a = ArenaCreate();
  StrView path = SVL("clanker_auth_resolve_test.json");
  remove("clanker_auth_resolve_test.json");

  // A missing store is not an error; it just resolves to nothing.
  CHECK(Auth_ResolveToken(&a, path, SVL("provider-a"), nullptr).Length == 0);

  SetTestEnv("CLANKER_TEST_AUTH_KEY", "env-key");
  CHECK(StrView_EqCStr(
      Auth_ResolveToken(&a, path, SVL("provider-a"), "CLANKER_TEST_AUTH_KEY"),
      "env-key"));

  // A stored key wins over the environment.
  Credential key = {};
  key.ApiKey = SVL("sk-stored");
  CHECK(AuthStore_Write(&a, path, SVL("provider-a"), &key));
  CHECK(StrView_EqCStr(
      Auth_ResolveToken(&a, path, SVL("provider-a"), "CLANKER_TEST_AUTH_KEY"),
      "sk-stored"));

  // An empty environment variable is treated as unset.
  CHECK(AuthStore_Delete(&a, path, SVL("provider-a")));
  SetTestEnv("CLANKER_TEST_AUTH_KEY", nullptr);
  CHECK(Auth_ResolveToken(&a, path, SVL("provider-a"), "CLANKER_TEST_AUTH_KEY")
            .Length == 0);

  remove("clanker_auth_resolve_test.json");
  ArenaRelease(&a);
}

static void TestTokenCache() {
  Arena a = ArenaCreate();
  AuthTokenCache cache = {};
  StrView cached = {};

  CHECK(!AuthTokenCache_Get(&cache, SVL("provider-a"), &cached));

  // The cache copies its value, so mutating the caller's buffer afterwards
  // cannot change what a later Get returns.
  char source[] = "cached-key";
  CHECK(AuthTokenCache_Store(&a, &cache, SVL("provider-a"), SV(source)));
  source[0] = 'X';
  CHECK(AuthTokenCache_Get(&cache, SVL("provider-a"), &cached));
  CHECK(StrView_EqCStr(cached, "cached-key"));
  StrView previous = cached;

  // One entry, keyed by provider.
  CHECK(!AuthTokenCache_Get(&cache, SVL("anthropic"), &cached));
  CHECK(AuthTokenCache_Store(&a, &cache, SVL("anthropic"), SVL("other-key")));
  for (u32 i = 0; i < previous.Length; i++) CHECK(previous.Data[i] == 0);
  CHECK(!AuthTokenCache_Get(&cache, SVL("provider-a"), &cached));
  CHECK(AuthTokenCache_Get(&cache, SVL("anthropic"), &cached));
  CHECK(StrView_EqCStr(cached, "other-key"));

  CHECK(!AuthTokenCache_Store(&a, &cache, SVL("anthropic"), StrView{}));
  CHECK(!AuthTokenCache_Store(&a, &cache, StrView{}, SVL("key")));

  // A signed-in token carries the account it is scoped to and its lapse time.
  CHECK(AuthTokenCache_Store(&a, &cache, SVL("openai-codex"), SVL("access"),
                             SVL("acct-1"), 1234));
  CHECK(AuthTokenCache_Get(&cache, SVL("openai-codex"), &cached));
  CHECK_ALL(StrView_EqCStr(cached, "access"),
            StrView_EqCStr(cache.AccountId, "acct-1"), cache.ExpiresAtMs == 1234);
  CHECK(AuthTokenCache_Store(&a, &cache, SVL("anthropic"), SVL("other-key")));
  CHECK_ALL(cache.AccountId.Length == 0, cache.ExpiresAtMs == 0);
  CHECK(AuthTokenCache_Get(&cache, SVL("anthropic"), &cached));

  previous = cached;
  AuthTokenCache_Invalidate(&cache);
  for (u32 i = 0; i < previous.Length; i++) CHECK(previous.Data[i] == 0);
  CHECK(!AuthTokenCache_Get(&cache, SVL("anthropic"), &cached));

  ArenaRelease(&a);
}

static void TestOAuthUrls() {
  Arena a = ArenaCreate();
  OAuthPkce pkce = {SVL("verifier"), SVL("chal-lenge_"), SVL("st8")};

  CHECK(StrView_EqCStr(
      OpenRouterOAuth_AuthorizeUrl(&a, &pkce, SVL("http://localhost:5123/callback")),
      "https://openrouter.ai/auth?callback_url=http%3A%2F%2Flocalhost%3A5123%2Fcallback"
      "&code_challenge=chal-lenge_&code_challenge_method=S256&key_label=Clanker&state=st8"));
  // Headless: no callback, and no state since OpenRouter only echoes it on a redirect.
  CHECK(StrView_EqCStr(OpenRouterOAuth_AuthorizeUrl(&a, &pkce, StrView{}),
                       "https://openrouter.ai/auth?code_challenge=chal-lenge_"
                       "&code_challenge_method=S256&key_label=Clanker"));

  OAuthPkce fresh = OAuthPkce_Create(&a);
  OAuthPkce other = OAuthPkce_Create(&a);
  CHECK_ALL(fresh.Verifier.Length == 43, fresh.State.Length == 22,
            StrView_Eq(fresh.Challenge, OAuthPkce_Challenge(&a, fresh.Verifier)),
            !StrView_Eq(fresh.Verifier, other.Verifier),
            !StrView_Eq(fresh.State, other.State));

  CHECK(StrView_EqCStr(OpenRouterOAuth_ExchangeBody(&a, SVL("c\"1"), SVL("v")),
                       "{\"code\":\"c\\\"1\",\"code_verifier\":\"v\","
                       "\"code_challenge_method\":\"S256\"}"));
  ArenaRelease(&a);
}

static void TestOAuthCallbackParse() {
  Arena a = ArenaCreate();
  StrView state = SVL("st8");
  StrView path = SVL(OPENROUTER_OAUTH_CALLBACK_PATH);

  OAuthCallback ok =
      OAuthCallback_Parse(&a, SVL("/callback?state=st8&code=a%2Bb+c"), path, state);
  CHECK_ALL(ok.Status == OAuthCallbackStatus::Code, StrView_EqCStr(ok.Code, "a+b c"));

  CHECK(OAuthCallback_Parse(&a, SVL("/favicon.ico"), path, state).Status ==
        OAuthCallbackStatus::WrongPath);
  CHECK(OAuthCallback_Parse(&a, SVL("/callbackx?code=1&state=st8"), path, state).Status ==
        OAuthCallbackStatus::WrongPath);
  CHECK(OAuthCallback_Parse(&a, SVL("/callback?code=1&state=other"), path, state).Status ==
        OAuthCallbackStatus::StateMismatch);
  CHECK(OAuthCallback_Parse(&a, SVL("/callback?code=1"), path, state).Status ==
        OAuthCallbackStatus::StateMismatch);
  CHECK(OAuthCallback_Parse(&a, SVL("/callback?state=st8&code="), path, state).Status ==
        OAuthCallbackStatus::MissingCode);

  OAuthCallback denied = OAuthCallback_Parse(
      &a, SVL("/callback?error=access_denied&error_description=User%20declined"), path,
      state);
  CHECK_ALL(denied.Status == OAuthCallbackStatus::Denied,
            StrView_EqCStr(denied.Error, "User declined"));

  // ChatGPT redirects to its own registered path.
  StrView codexPath = SVL(OPENAI_CODEX_OAUTH_CALLBACK_PATH);
  OAuthCallback codex =
      OAuthCallback_Parse(&a, SVL("/auth/callback?code=c1&state=st8"), codexPath, state);
  CHECK_ALL(codex.Status == OAuthCallbackStatus::Code, StrView_EqCStr(codex.Code, "c1"));
  CHECK(OAuthCallback_Parse(&a, SVL("/callback?code=c1&state=st8"), codexPath, state)
            .Status == OAuthCallbackStatus::WrongPath);
  ArenaRelease(&a);
}

static void TestOAuthExchangeParse() {
  Arena a = ArenaCreate();
  OAuthKeyResult ok = OpenRouterOAuth_ParseExchange(
      &a, 200, SVL("{\"key\":\"sk-or-v1-abc\",\"user_id\":\"u\"}"), StrView{});
  CHECK_ALL(ok.Ok, StrView_EqCStr(ok.Key, "sk-or-v1-abc"));

  OAuthKeyResult noKey = OpenRouterOAuth_ParseExchange(&a, 200, SVL("{}"), StrView{});
  CHECK(!noKey.Ok);

  OAuthKeyResult rejected = OpenRouterOAuth_ParseExchange(
      &a, 403, SVL("{\"error\":{\"code\":403,\"message\":\"Invalid code\"}}"), StrView{});
  CHECK_ALL(!rejected.Ok, StrView_EqCStr(rejected.Error, "HTTP 403: Invalid code"));

  OAuthKeyResult opaque = OpenRouterOAuth_ParseExchange(&a, 502, SVL("<html>"), StrView{});
  CHECK(StrView_EqCStr(opaque.Error, "HTTP 502"));

  OAuthKeyResult transport =
      OpenRouterOAuth_ParseExchange(&a, -1, StrView{}, SVL("could not resolve host"));
  CHECK_ALL(!transport.Ok, StrView_EqCStr(transport.Error, "could not resolve host"));
  ArenaRelease(&a);
}

// An unsigned token with the given claims; only the payload is ever read.
static StrView FakeJwt(Arena* a, const char* claims) {
  StrBuf jwt = StrBuf_Make(a, 256);
  StrBuf_AppendCStr(&jwt, "eyJhbGciOiJub25lIn0.");
  StrBuf_Append(&jwt, Base64UrlEncode(a, (const byte*)claims, (u32)strlen(claims)));
  StrBuf_AppendCStr(&jwt, ".sig");
  return StrBuf_View(&jwt);
}

static StrView TokenBody(Arena* a, StrView access, const char* rest) {
  StrBuf body = StrBuf_Make(a, 256);
  StrBuf_AppendCStr(&body, "{\"access_token\":\"");
  StrBuf_Append(&body, access);
  StrBuf_AppendChar(&body, '"');
  StrBuf_AppendCStr(&body, rest);
  StrBuf_AppendChar(&body, '}');
  return StrBuf_View(&body);
}

static void TestCodexOAuth() {
  Arena a = ArenaCreate();
  OAuthPkce pkce = {SVL("verifier"), SVL("chal-lenge_"), SVL("st8")};
  CHECK(StrView_EqCStr(
      OpenAICodexOAuth_AuthorizeUrl(&a, &pkce),
      "https://auth.openai.com/oauth/authorize?response_type=code"
      "&client_id=" OPENAI_CODEX_OAUTH_CLIENT_ID
      "&redirect_uri=http%3A%2F%2Flocalhost%3A1455%2Fauth%2Fcallback"
      "&scope=openid%20profile%20email%20offline_access"
      "&code_challenge=chal-lenge_&code_challenge_method=S256&state=st8"
      "&id_token_add_organizations=true&codex_cli_simplified_flow=true"
      "&originator=clanker"));
  CHECK(StrView_EqCStr(
      OpenAICodexOAuth_ExchangeBody(&a, SVL("c/1&x"), SVL("v")),
      "grant_type=authorization_code&client_id=" OPENAI_CODEX_OAUTH_CLIENT_ID
      "&code=c%2F1%26x&code_verifier=v"
      "&redirect_uri=http%3A%2F%2Flocalhost%3A1455%2Fauth%2Fcallback"));
  CHECK(StrView_EqCStr(OpenAICodexOAuth_RefreshBody(&a, SVL("r+1")),
                       "grant_type=refresh_token&refresh_token=r%2B1"
                       "&client_id=" OPENAI_CODEX_OAUTH_CLIENT_ID));

  StrView access = FakeJwt(
      &a, "{\"exp\":2000000000,\"https://api.openai.com/auth\":"
          "{\"chatgpt_account_id\":\"acct-1\"}}");
  CHECK_ALL(StrView_EqCStr(OpenAICodexOAuth_AccountId(&a, access), "acct-1"),
            OpenAICodexOAuth_TokenExpiry(&a, access) == 2000000000000ull,
            OpenAICodexOAuth_AccountId(&a, SVL("not-a-jwt")).Length == 0,
            OpenAICodexOAuth_TokenExpiry(&a, SVL("a.!!.c")) == 0);

  const u64 now = 1700000000000ull;
  OpenAICodexTokens fresh = OpenAICodexOAuth_ParseTokens(
      &a, 200, TokenBody(&a, access, ",\"refresh_token\":\"r2\",\"expires_in\":3600"),
      StrView{}, StrView{}, now);
  CHECK_ALL(fresh.Ok, StrView_Eq(fresh.Access, access),
            StrView_EqCStr(fresh.Refresh, "r2"), StrView_EqCStr(fresh.AccountId, "acct-1"),
            fresh.ExpiresAtMs == now + 3600000);

  // A refresh that does not rotate keeps the previous refresh token, and
  // without expires_in the token's own exp claim decides.
  OpenAICodexTokens kept =
      OpenAICodexOAuth_ParseTokens(&a, 200, TokenBody(&a, access, ""), StrView{},
                                   SVL("r1"), now);
  CHECK_ALL(kept.Ok, StrView_EqCStr(kept.Refresh, "r1"),
            kept.ExpiresAtMs == 2000000000000ull);
  CHECK(StrView_EqCStr(OpenAICodexOAuth_ParseTokens(&a, 200, TokenBody(&a, access, ""),
                                                    StrView{}, StrView{}, now)
                           .Error,
                       "the response did not contain a refresh token"));

  StrView noAccount = FakeJwt(&a, "{\"exp\":2000000000}");
  OpenAICodexTokens anonymous = OpenAICodexOAuth_ParseTokens(
      &a, 200, TokenBody(&a, noAccount, ",\"refresh_token\":\"r\""), StrView{},
      StrView{}, now);
  CHECK_ALL(!anonymous.Ok, StrView_EqCStr(anonymous.Error,
                                          "the access token does not identify a "
                                          "ChatGPT account"));
  StrView noExpiry = FakeJwt(
      &a, "{\"https://api.openai.com/auth\":{\"chatgpt_account_id\":\"acct-1\"}}");
  CHECK(StrView_EqCStr(
      OpenAICodexOAuth_ParseTokens(&a, 200, TokenBody(&a, noExpiry, ",\"refresh_token\":\"r\""),
                                   StrView{}, StrView{}, now)
          .Error,
      "the response did not say when the token expires"));
  CHECK(StrView_EqCStr(
      OpenAICodexOAuth_ParseTokens(&a, 200, SVL("{}"), StrView{}, SVL("r"), now).Error,
      "the response did not contain an access token"));

  CHECK(StrView_EqCStr(
      OpenAICodexOAuth_ParseTokens(
          &a, 400,
          SVL("{\"error\":\"invalid_grant\",\"error_description\":\"Refresh token expired\"}"),
          StrView{}, SVL("r"), now)
          .Error,
      "HTTP 400: Refresh token expired"));
  CHECK(StrView_EqCStr(
      OpenAICodexOAuth_ParseTokens(&a, 401,
                                   SVL("{\"error\":{\"message\":\"Bad token\",\"code\":\"x\"}}"),
                                   StrView{}, StrView{}, now)
          .Error,
      "HTTP 401: Bad token"));
  CHECK(StrView_EqCStr(
      OpenAICodexOAuth_ParseTokens(&a, 400, SVL("{\"error\":\"invalid_grant\"}"), StrView{},
                                   StrView{}, now)
          .Error,
      "HTTP 400: invalid_grant"));
  CHECK(StrView_EqCStr(
      OpenAICodexOAuth_ParseTokens(&a, 502, SVL("<html>"), StrView{}, StrView{}, now).Error,
      "HTTP 502"));
  CHECK(StrView_EqCStr(OpenAICodexOAuth_ParseTokens(&a, -1, StrView{}, SVL("timed out"),
                                                    StrView{}, now)
                           .Error,
                       "timed out"));
  CHECK(StrView_EqCStr(
      OpenAICodexOAuth_ParseTokens(&a, -1, StrView{}, StrView{}, StrView{}, now).Error,
      "the request failed"));
  ArenaRelease(&a);
}

// The code a pasted value yields for state "st8", or empty when it is refused.
static StrView PastedCode(Arena* a, const char* input) {
  StrView code = {};
  StrView error = {};
  bool ok = OpenAICodexOAuth_ParsePastedInput(a, SV(input), SVL("st8"), &code, &error);
  return ok && !error.Length ? code : StrView{};
}

// Why a pasted value was refused, or empty when it was accepted or a refusal
// still leaked a code.
static StrView PastedError(Arena* a, const char* input) {
  StrView code = {};
  StrView error = {};
  bool ok = OpenAICodexOAuth_ParsePastedInput(a, SV(input), SVL("st8"), &code, &error);
  return ok || code.Length ? StrView{} : error;
}

static void TestCodexPastedInput() {
  Arena a = ArenaCreate();
  CHECK_ALL(
      StrView_EqCStr(
          PastedCode(&a, "http://localhost:1455/auth/callback?code=c%2B1&state=st8"), "c+1"),
      StrView_EqCStr(PastedCode(&a, "  c2#st8 \n"), "c2"),
      StrView_EqCStr(PastedCode(&a, "code=c3&state=st8"), "c3"),
      StrView_EqCStr(PastedCode(&a, "?code=c4"), "c4"),
      StrView_EqCStr(PastedCode(&a, "c5"), "c5"));

  const char* mismatch = "this authorization belongs to a different sign-in attempt";
  CHECK_ALL(
      StrView_EqCStr(
          PastedError(&a, "http://localhost:1455/auth/callback?code=c6&state=other"),
          mismatch),
      StrView_EqCStr(PastedError(&a, "c7#other"), mismatch),
      StrView_EqCStr(PastedError(&a,
                                 "http://localhost:1455/auth/callback?error=access_denied"
                                 "&error_description=User%20declined"),
                     "sign-in was denied: User declined"),
      StrView_EqCStr(PastedError(&a, "http://localhost:1455/auth/callback?state=st8"),
                     "no authorization code was found"),
      StrView_EqCStr(PastedError(&a, "   "), "no authorization code was found"));

  StrView decoded = {};
  CHECK(Base64UrlDecode(&a, SVL("aGk_Pz4-"), &decoded));
  CHECK(StrView_EqCStr(decoded, "hi??>>"));
  CHECK(Base64UrlDecode(&a, SVL("aGk="), &decoded));
  CHECK(StrView_EqCStr(decoded, "hi"));
  CHECK(!Base64UrlDecode(&a, SVL("a"), &decoded));
  CHECK(!Base64UrlDecode(&a, SVL("a.bc"), &decoded));
  ArenaRelease(&a);
}

struct LoopbackClient {
  StrView BaseUrl;
  HttpResponse Stray;
  HttpResponse Callback;
  Arena Mem;
};

static void RunLoopbackClient(void* arg) {
  LoopbackClient* client = (LoopbackClient*)arg;
  HttpRequest request = {};
  request.Method = SVL("GET");
  StrBuf stray = StrBuf_Make(&client->Mem, 64);
  StrBuf_Append(&stray, client->BaseUrl);
  StrBuf_AppendCStr(&stray, "/favicon.ico");
  request.Url = StrBuf_View(&stray);
  client->Stray = Http_Fetch(&client->Mem, &request, nullptr);

  StrBuf callback = StrBuf_Make(&client->Mem, 64);
  StrBuf_Append(&callback, client->BaseUrl);
  StrBuf_AppendCStr(&callback, "/callback?code=xyz&state=st8");
  request.Url = StrBuf_View(&callback);
  client->Callback = Http_Fetch(&client->Mem, &request, nullptr);
}

// The listener is real (127.0.0.1, OS-assigned port), driven by curl as the
// browser stand-in.
static void TestLoopbackServer() {
  Arena a = ArenaCreate();
  Http_GlobalInit();
  LoopbackServer server;
  CHECK(LoopbackServer_Open(&server));
  if (server.Socket == LOOPBACK_INVALID_SOCKET) {
    Http_GlobalShutdown();
    ArenaRelease(&a);
    return;
  }
  CHECK(server.Port != 0);

  // Nothing connects, so these return without blocking for long.
  volatile i32 aborted = 1;
  LoopbackRequest request;
  CHECK(LoopbackServer_Accept(&server, &a, &aborted, 60000, &request) ==
        LoopbackWait::Aborted);
  CHECK(LoopbackServer_Accept(&server, &a, nullptr, 0, &request) == LoopbackWait::TimedOut);

  LoopbackClient client = {};
  client.Mem = ArenaCreate();
  StrBuf base = StrBuf_Make(&a, 32);
  StrBuf_AppendCStr(&base, "http://127.0.0.1:");
  StrBuf_AppendU64(&base, server.Port);
  client.BaseUrl = StrBuf_View(&base);
  Thread thread = {};
  CHECK(Thread_Start(&thread, RunLoopbackClient, &client));

  CHECK(LoopbackServer_Accept(&server, &a, nullptr, 10000, &request) ==
        LoopbackWait::Request);
  CHECK(StrView_EqCStr(request.Target, "/favicon.ico"));
  LoopbackRequest_Respond(&request, 404, SVL("Not found."));

  CHECK(LoopbackServer_Accept(&server, &a, nullptr, 10000, &request) ==
        LoopbackWait::Request);
  OAuthCallback parsed = OAuthCallback_Parse(&a, request.Target,
                                             SVL(OPENROUTER_OAUTH_CALLBACK_PATH), SVL("st8"));
  CHECK_ALL(parsed.Status == OAuthCallbackStatus::Code, StrView_EqCStr(parsed.Code, "xyz"));
  LoopbackRequest_Respond(&request, 200, SVL("<p>signed in</p>"));

  Thread_Join(&thread);
  CHECK_ALL(client.Stray.Status == 404, client.Callback.Status == 200,
            StrView_EqCStr(client.Callback.Body, "<p>signed in</p>"));

  LoopbackServer_Close(&server);
  ArenaRelease(&client.Mem);
  Http_GlobalShutdown();
  ArenaRelease(&a);
}

void TestAuth() {
  TestStore();
  TestOAuthStore();
  TestStoreFailurePreservesContent();
  TestResolve();
  TestTokenCache();
  TestOAuthUrls();
  TestOAuthCallbackParse();
  TestOAuthExchangeParse();
  TestCodexOAuth();
  TestCodexPastedInput();
  TestLoopbackServer();
}
