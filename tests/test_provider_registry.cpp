#include <string.h>

#include "app/provider_registry.h"
#include "auth/oauth.h"
#include "base/arena.h"
#include "check.h"
#include "llm/anthropic.h"
#include "llm/openai.h"

struct ModelsFetchFixture {
  HttpResponse Responses[4];
  u32 ResponseCount;
  u32 Calls;
  StrView Urls[4];
  StrView ApiKeys[4];
  StrView Authorizations[4];
  StrView Betas[4];
  StrView UserAgents[4];
  StrView Apps[4];
  StrView Versions[4];
  volatile i32* AbortSeen;
  bool RequestsValid = true;
};

static StrView FindHeader(const HttpRequest* request, const char* name) {
  for (u32 i = 0; i < request->HeaderCount; i++) {
    if (StrView_EqCStr(request->Headers[i].Name, name)) return request->Headers[i].Value;
  }
  return {};
}

static HttpResponse FetchModelsFixture(void* user, Arena*, const HttpRequest* request,
                                       volatile i32* abort) {
  ModelsFetchFixture* fixture = (ModelsFetchFixture*)user;
  u32 call = fixture->Calls++;
  if (call >= fixture->ResponseCount) {
    fixture->RequestsValid = false;
    return {};
  }
  fixture->RequestsValid =
      fixture->RequestsValid && StrView_EqCStr(request->Method, "GET") &&
      request->Body.Length == 0;
  fixture->Urls[call] = request->Url;
  fixture->ApiKeys[call] = FindHeader(request, "x-api-key");
  fixture->Authorizations[call] = FindHeader(request, "authorization");
  fixture->Betas[call] = FindHeader(request, "anthropic-beta");
  fixture->UserAgents[call] = FindHeader(request, "user-agent");
  fixture->Apps[call] = FindHeader(request, "x-app");
  fixture->Versions[call] = FindHeader(request, "anthropic-version");
  fixture->AbortSeen = abort;
  return fixture->Responses[call];
}

static void TestAnthropicModelDiscovery() {
  Arena arena = ArenaCreate();
  ModelsFetchFixture fixture = {};
  fixture.ResponseCount = 2;
  fixture.Responses[0].Status = 200;
  fixture.Responses[0].Body =
      SVL("{\"data\":[{\"id\":\"claude-sonnet-4-6\",\"display_name\":\"Live Sonnet\","
          "\"max_input_tokens\":1000000,\"max_tokens\":64000,"
          "\"capabilities\":{\"thinking\":{\"supported\":true}}}],"
          "\"has_more\":true,\"last_id\":\"cursor /one\"}");
  fixture.Responses[1].Status = 200;
  fixture.Responses[1].Body =
      SVL("{\"data\":[{\"id\":\"claude-extra\",\"display_name\":\"Claude Extra\"},"
          "{\"id\":\"claude-sonnet-4-6\",\"display_name\":\"Duplicate Sonnet\"}],"
          "\"has_more\":false,\"last_id\":\"claude-extra\"}");
  volatile i32 abort = 0;

  ProviderModelList live =
      Anthropic_ListModelsWithFetch(
          &arena,
          ProviderAuth{ProviderAuthKind::ApiKey, SVL("sk-ant-api")},
          &abort, FetchModelsFixture, &fixture);
  CHECK_ALL(live.Status == ProviderModelListStatus::Success, live.Count == 2,
            fixture.RequestsValid, fixture.Calls == 2, fixture.AbortSeen == &abort);
  CHECK_ALL(
      StrView_EqCStr(fixture.Urls[0], "https://api.anthropic.com/v1/models?limit=1000"),
      StrView_EqCStr(
          fixture.Urls[1],
          "https://api.anthropic.com/v1/models?limit=1000&after_id=cursor%20%2Fone"),
      StrView_EqCStr(fixture.ApiKeys[0], "sk-ant-api"),
      fixture.Authorizations[0].Length == 0,
      StrView_EqCStr(fixture.Versions[0], "2023-06-01"));
  CHECK_ALL(StrView_EqCStr(live.Models[0].Name, "Live Sonnet"),
            live.Models[0].ContextWindow == 1000000, live.Models[0].MaxTokens == 64000,
            live.Models[0].Reasoning,
            Model_SupportsEffort(&live.Models[0], ModelEffort::Max),
            StrView_EqCStr(live.Models[1].Id, "claude-extra"));

  const ProviderSpec* spec = ProviderRegistry_Find(SVL("anthropic"));
  ProviderModelList merged =
      ProviderModelList_Merge(&arena, live, spec->FallbackModels, spec->FallbackModelCount);
  CHECK_ALL(merged.Count == 4, StrView_EqCStr(merged.Models[0].Name, "Live Sonnet"),
            StrView_EqCStr(merged.Models[2].Id, "claude-opus-4-6"),
            StrView_EqCStr(merged.Models[3].Id, "claude-haiku-4-5"));
  ArenaRelease(&arena);
}

static void TestAnthropicModelDiscoveryOAuthAndEmpty() {
  Arena arena = ArenaCreate();
  ModelsFetchFixture fixture = {};
  fixture.ResponseCount = 1;
  fixture.Responses[0].Status = 200;
  fixture.Responses[0].Body = SVL("{\"data\":[],\"has_more\":false,\"last_id\":null}");
  ProviderModelList live = Anthropic_ListModelsWithFetch(
      &arena,
      ProviderAuth{ProviderAuthKind::ApiKey, SVL("sk-ant-oat01-test")},
      nullptr, FetchModelsFixture, &fixture);
  CHECK_ALL(live.Status == ProviderModelListStatus::Success, live.Count == 0,
            fixture.RequestsValid, fixture.Calls == 1, fixture.ApiKeys[0].Length == 0);
  CHECK(StrView_EqCStr(fixture.Authorizations[0], "Bearer sk-ant-oat01-test"));
  CHECK_ALL(StrView_EqCStr(fixture.Betas[0], "claude-code-20250219,oauth-2025-04-20"),
            StrView_EqCStr(fixture.UserAgents[0], "claude-cli/2.1.75"),
            StrView_EqCStr(fixture.Apps[0], "cli"));

  const ProviderSpec* spec = ProviderRegistry_Find(SVL("anthropic"));
  ProviderModelList merged =
      ProviderModelList_Merge(&arena, live, spec->FallbackModels, spec->FallbackModelCount);
  CHECK(merged.Count == spec->FallbackModelCount);
  ArenaRelease(&arena);
}

static void TestAnthropicModelDiscoveryFailures() {
  Arena arena = ArenaCreate();
  ProviderModelList missing =
      Anthropic_ListModelsWithFetch(&arena, {}, nullptr, FetchModelsFixture, nullptr);
  CHECK_ALL(missing.Status == ProviderModelListStatus::Unavailable,
            missing.Error.Length != 0);

  volatile i32 abort = 1;
  ModelsFetchFixture unused = {};
  ProviderModelList aborted = Anthropic_ListModelsWithFetch(
      &arena, ProviderAuth{ProviderAuthKind::ApiKey, SVL("token")}, &abort,
      FetchModelsFixture, &unused);
  CHECK_ALL(aborted.Status == ProviderModelListStatus::Unavailable, unused.Calls == 0);

  ModelsFetchFixture transport = {};
  transport.ResponseCount = 1;
  transport.Responses[0].Status = -1;
  transport.Responses[0].Error = SVL("offline");
  ProviderModelList unavailable = Anthropic_ListModelsWithFetch(
      &arena, ProviderAuth{ProviderAuthKind::ApiKey, SVL("token")}, nullptr,
      FetchModelsFixture, &transport);
  CHECK_ALL(unavailable.Status == ProviderModelListStatus::Unavailable,
            StrView_EqCStr(unavailable.Error, "offline"), transport.RequestsValid);

  ModelsFetchFixture http = {};
  http.ResponseCount = 1;
  http.Responses[0].Status = 401;
  ProviderModelList failed =
      Anthropic_ListModelsWithFetch(
          &arena, ProviderAuth{ProviderAuthKind::ApiKey, SVL("token")},
          nullptr, FetchModelsFixture, &http);
  CHECK_ALL(failed.Status == ProviderModelListStatus::Failed, failed.Error.Length != 0,
            http.RequestsValid);

  ModelsFetchFixture malformed = {};
  malformed.ResponseCount = 1;
  malformed.Responses[0].Status = 200;
  malformed.Responses[0].Body = SVL("{\"data\":[],\"has_more\":true}");
  ProviderModelList badPage = Anthropic_ListModelsWithFetch(
      &arena, ProviderAuth{ProviderAuthKind::ApiKey, SVL("token")}, nullptr,
      FetchModelsFixture, &malformed);
  CHECK_ALL(badPage.Status == ProviderModelListStatus::Failed, malformed.RequestsValid);

  ModelsFetchFixture repeated = {};
  repeated.ResponseCount = 2;
  repeated.Responses[0].Status = 200;
  repeated.Responses[0].Body =
      SVL("{\"data\":[],\"has_more\":true,\"last_id\":\"same\"}");
  repeated.Responses[1] = repeated.Responses[0];
  ProviderModelList loop = Anthropic_ListModelsWithFetch(
      &arena, ProviderAuth{ProviderAuthKind::ApiKey, SVL("token")}, nullptr,
      FetchModelsFixture, &repeated);
  CHECK_ALL(loop.Status == ProviderModelListStatus::Failed, repeated.RequestsValid,
            repeated.Calls == 2);
  ArenaRelease(&arena);
}

void TestProviderRegistry() {
  CHECK_ALL(ProviderRegistry_Count() == 2,
            ProviderRegistry_At(ProviderRegistry_Count()) == nullptr,
            ProviderRegistry_Find(SVL("nonexistent")) == nullptr,
            ProviderRegistry_Find(SVL("missing")) == nullptr);

  const ProviderSpec* spec = ProviderRegistry_Find(SVL("anthropic"));
  CHECK(spec != nullptr);
  CHECK_ALL(StrView_EqCStr(spec->Name, "Anthropic"),
            StrView_EqCStr(spec->DefaultModel, "claude-sonnet-4-6"),
            strcmp(spec->ApiKeyEnvVar, "ANTHROPIC_API_KEY") == 0,
            spec->CreateProvider != nullptr, spec->CreateModel != nullptr,
            spec->ListModels != nullptr, spec->FallbackModels != nullptr,
            spec->FallbackModelCount == 3);

  Arena arena = ArenaCreate();
  char manualId[] = "claude-manual";
  Model manual = spec->CreateModel(&arena, StrView{manualId, 13});
  manualId[0] = 'x';
  CHECK_ALL(StrView_EqCStr(manual.Id, "claude-manual"),
            StrView_EqCStr(manual.Name, "claude-manual"),
            StrView_Eq(manual.Provider, spec->Id),
            manual.ContextWindow == 0, manual.EffortMask == 0);

  Model fallback = spec->CreateModel(&arena, spec->FallbackModels[0].Id);
  CHECK_ALL(StrView_Eq(fallback.Id, spec->DefaultModel),
            StrView_EqCStr(fallback.Name, "Claude Sonnet 4.6"),
            fallback.ContextWindow == 1000000,
            fallback.MaxTokens == 64000,
            Model_SupportsEffort(&fallback, ModelEffort::Medium),
            Model_SupportsEffort(&fallback, ModelEffort::Max));

  Provider provider = spec->CreateProvider(&arena);
  CHECK(provider.Stream != nullptr);

  const OAuthConfig* oauth = OAuth_GetConfig(spec->Id);
  CHECK(oauth != nullptr);
  CHECK(StrView_Eq(oauth->Id, spec->Id));

  const ProviderSpec* openai = ProviderRegistry_Find(SVL("openai"));
  CHECK(openai != nullptr);
  CHECK_ALL(
      StrView_EqCStr(openai->Name, "OpenAI"),
      StrView_EqCStr(openai->DefaultModel, "gpt-5.6-sol"),
      strcmp(openai->ApiKeyEnvVar, "OPENAI_API_KEY") == 0,
      openai->CreateProvider != nullptr, openai->CreateModel != nullptr,
      openai->ListModels != nullptr, openai->FallbackModels != nullptr,
      openai->FallbackModelCount == 5);
  Model openaiDefault =
      openai->CreateModel(&arena, openai->DefaultModel);
  CHECK_ALL(
      StrView_EqCStr(openaiDefault.Provider, "openai"),
      openaiDefault.ContextWindow == 272000,
      Model_SupportsEffort(&openaiDefault, ModelEffort::Ultra));
  Provider openaiProvider = openai->CreateProvider(&arena);
  CHECK(openaiProvider.Stream != nullptr);
  const OAuthConfig* openaiOAuth = OAuth_GetConfig(openai->Id);
  CHECK(openaiOAuth != nullptr);
  CHECK(StrView_Eq(openaiOAuth->Id, openai->Id));

  ArenaRelease(&arena);
  TestAnthropicModelDiscovery();
  TestAnthropicModelDiscoveryOAuthAndEmpty();
  TestAnthropicModelDiscoveryFailures();
}
