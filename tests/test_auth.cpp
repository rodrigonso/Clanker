#include <stdio.h>
#include <stdlib.h>

#include "auth/openrouter_oauth.h"
#include "auth/resolve.h"
#include "auth/store.h"
#include "base/arena.h"
#include "base/config_file.h"
#include "base/str.h"
#include "base/thread.h"
#include "check.h"
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

  // An entry whose type is not api_key is dropped rather than half-parsed.
  CHECK(ConfigFile_WriteAtomic(
      &a, path, SVL("{\"openai\":{\"type\":\"oauth\",\"access\":\"tok\"}}")));
  CHECK(!AuthStore_Read(&a, path, SVL("provider-a"), &gone));

  remove("clanker_auth_test.json");
  ArenaRelease(&a);
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

  // One entry, keyed by provider.
  CHECK(!AuthTokenCache_Get(&cache, SVL("anthropic"), &cached));
  CHECK(AuthTokenCache_Store(&a, &cache, SVL("anthropic"), SVL("other-key")));
  CHECK(!AuthTokenCache_Get(&cache, SVL("provider-a"), &cached));
  CHECK(AuthTokenCache_Get(&cache, SVL("anthropic"), &cached));
  CHECK(StrView_EqCStr(cached, "other-key"));

  CHECK(!AuthTokenCache_Store(&a, &cache, SVL("anthropic"), StrView{}));
  CHECK(!AuthTokenCache_Store(&a, &cache, StrView{}, SVL("key")));

  AuthTokenCache_Invalidate(&cache);
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

  OAuthCallback ok = OAuthCallback_Parse(&a, SVL("/callback?state=st8&code=a%2Bb+c"), state);
  CHECK_ALL(ok.Status == OAuthCallbackStatus::Code, StrView_EqCStr(ok.Code, "a+b c"));

  CHECK(OAuthCallback_Parse(&a, SVL("/favicon.ico"), state).Status ==
        OAuthCallbackStatus::WrongPath);
  CHECK(OAuthCallback_Parse(&a, SVL("/callbackx?code=1&state=st8"), state).Status ==
        OAuthCallbackStatus::WrongPath);
  CHECK(OAuthCallback_Parse(&a, SVL("/callback?code=1&state=other"), state).Status ==
        OAuthCallbackStatus::StateMismatch);
  CHECK(OAuthCallback_Parse(&a, SVL("/callback?code=1"), state).Status ==
        OAuthCallbackStatus::StateMismatch);
  CHECK(OAuthCallback_Parse(&a, SVL("/callback?state=st8&code="), state).Status ==
        OAuthCallbackStatus::MissingCode);

  OAuthCallback denied = OAuthCallback_Parse(
      &a, SVL("/callback?error=access_denied&error_description=User%20declined"), state);
  CHECK_ALL(denied.Status == OAuthCallbackStatus::Denied,
            StrView_EqCStr(denied.Error, "User declined"));
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
  OAuthCallback parsed = OAuthCallback_Parse(&a, request.Target, SVL("st8"));
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
  TestResolve();
  TestTokenCache();
  TestOAuthUrls();
  TestOAuthCallbackParse();
  TestOAuthExchangeParse();
  TestLoopbackServer();
}
