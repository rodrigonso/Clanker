#include <stdio.h>
#include <stdlib.h>

#include "auth/resolve.h"
#include "auth/store.h"
#include "base/arena.h"
#include "base/config_file.h"
#include "base/str.h"
#include "check.h"

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

void TestAuth() {
  TestStore();
  TestResolve();
  TestTokenCache();
}
