#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "app/model_cache.h"
#include "app/provider_registry.h"
#include "base/arena.h"
#include "base/config_file.h"
#include "check.h"
#include "llm/responses.h"

struct ModelsFetchFixture {
  HttpResponse Responses[2];
  u32 ResponseCount;
  u32 Calls;
  StrView Urls[2];
  StrView Authorizations[2];
  StrView Accepts[2];
  StrView AccountIds[2];
  StrView Originators[2];
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
  fixture->RequestsValid = fixture->RequestsValid &&
                           StrView_EqCStr(request->Method, "GET") &&
                           request->Body.Length == 0;
  fixture->Urls[call] = request->Url;
  fixture->Authorizations[call] = FindHeader(request, "authorization");
  fixture->Accepts[call] = FindHeader(request, "accept");
  fixture->AccountIds[call] = FindHeader(request, "chatgpt-account-id");
  fixture->Originators[call] = FindHeader(request, "originator");
  fixture->AbortSeen = abort;
  return fixture->Responses[call];
}

static void TestModelDiscoveryFailures() {
  Arena arena = ArenaCreate();
  const ResponsesEndpoint* endpoint =
      ProviderRegistry_Find(SVL("openrouter"))->Endpoint;
  ProviderAuth key = {SVL("test-key"), {}};

  ProviderModelList noEndpoint = Responses_ListModelsWithFetch(
      &arena, nullptr, key, nullptr, FetchModelsFixture, nullptr);
  CHECK_ALL(noEndpoint.Status == ProviderModelListStatus::Unavailable,
            noEndpoint.Error.Length != 0);

  // The Responses API authenticates every caller; there is no anonymous path.
  ProviderModelList anonymous = Responses_ListModelsWithFetch(
      &arena, endpoint, {}, nullptr, FetchModelsFixture, nullptr);
  CHECK(anonymous.Status == ProviderModelListStatus::Unavailable);

  volatile i32 abort = 1;
  ModelsFetchFixture unused = {};
  ProviderModelList aborted = Responses_ListModelsWithFetch(
      &arena, endpoint, key, &abort, FetchModelsFixture, &unused);
  CHECK_ALL(aborted.Status == ProviderModelListStatus::Unavailable, unused.Calls == 0);

  ModelsFetchFixture transport = {};
  transport.ResponseCount = 1;
  transport.Responses[0].Status = -1;
  transport.Responses[0].Error = SVL("offline");
  ProviderModelList unavailable = Responses_ListModelsWithFetch(
      &arena, endpoint, key, nullptr, FetchModelsFixture, &transport);
  CHECK_ALL(unavailable.Status == ProviderModelListStatus::Unavailable,
            StrView_EqCStr(unavailable.Error, "offline"), transport.RequestsValid);

  ModelsFetchFixture http = {};
  http.ResponseCount = 1;
  http.Responses[0].Status = 401;
  ProviderModelList failed = Responses_ListModelsWithFetch(
      &arena, endpoint, key, nullptr, FetchModelsFixture, &http);
  CHECK_ALL(failed.Status == ProviderModelListStatus::Failed,
            failed.Error.Length != 0, http.RequestsValid);

  ModelsFetchFixture malformed = {};
  malformed.ResponseCount = 1;
  malformed.Responses[0].Status = 200;
  malformed.Responses[0].Body = SVL("{\"object\":\"list\"}");
  ProviderModelList badBody = Responses_ListModelsWithFetch(
      &arena, endpoint, key, nullptr, FetchModelsFixture, &malformed);
  CHECK_ALL(badBody.Status == ProviderModelListStatus::Failed,
            badBody.Error.Length != 0, malformed.RequestsValid);
  ArenaRelease(&arena);
}

static void TestModelDiscovery() {
  Arena arena = ArenaCreate();
  const ProviderSpec* spec = ProviderRegistry_Find(SVL("openrouter"));
  ModelsFetchFixture fixture = {};
  fixture.ResponseCount = 1;
  fixture.Responses[0].Status = 200;
  fixture.Responses[0].Body = SVL(
      "{\"data\":["
      "{\"id\":\"anthropic/claude-sonnet-4.5\",\"name\":\"Claude Sonnet 4.5\","
      "\"context_length\":1000000,\"top_provider\":{\"max_completion_tokens\":"
      "64000},\"supported_parameters\":[\"reasoning\",\"tools\"],"
      "\"reasoning\":{\"supported_efforts\":[\"low\",\"high\"],"
      "\"default_effort\":\"high\"}},"
      "{\"id\":\"qwen/qwen3-max\",\"name\":\"Qwen3 Max\",\"context_length\":"
      "262144,\"top_provider\":{\"max_completion_tokens\":65536},"
      "\"supported_parameters\":[\"tools\"]},"
      "{\"id\":\"vendor/audio-model\",\"name\":\"Audio\","
      "\"supported_parameters\":[\"tools\"],\"architecture\":{"
      "\"input_modalities\":[\"audio\"],\"output_modalities\":[\"text\"]}},"
      "{\"id\":\"mistralai/mistral-medium\",\"name\":\"Mistral Medium\","
      "\"context_length\":131072,\"supported_parameters\":[\"tools\"]}"
      "]}");
  ProviderAuth auth = {SVL("test-key"), {}};

  ProviderModelList live = Responses_ListModelsWithFetch(
      &arena, spec->Endpoint, auth, nullptr, FetchModelsFixture, &fixture);
  // A base URL that already ends in "/v1" does not get a second one.
  CHECK_ALL(live.Status == ProviderModelListStatus::Success, live.Count == 3,
            fixture.RequestsValid,
            StrView_EqCStr(fixture.Urls[0],
                           "https://openrouter.ai/api/v1/models"));

  StrBuf expectedAuth = StrBuf_Make(&arena, auth.Token.Length + 8);
  StrBuf_AppendCStr(&expectedAuth, "Bearer ");
  StrBuf_Append(&expectedAuth, auth.Token);
  CHECK_ALL(StrView_Eq(fixture.Authorizations[0], StrBuf_View(&expectedAuth)),
            StrView_EqCStr(fixture.Accepts[0], "application/json"));

  // OpenRouter's catalog decides availability and exact reasoning controls.
  // Its max-output ceiling is deliberately not adopted as a request budget.
  CHECK_ALL(StrView_EqCStr(live.Models[0].Id, "anthropic/claude-sonnet-4.5"),
            live.Models[0].ContextWindow == 1000000,
            live.Models[0].MaxTokens == 0, live.Models[0].Reasoning,
            Model_SupportsEffort(&live.Models[0], ModelEffort::Low),
            !Model_SupportsEffort(&live.Models[0], ModelEffort::Medium),
            Model_SupportsEffort(&live.Models[0], ModelEffort::High),
            live.Models[0].DefaultEffort == ModelEffort::High,
            Model_MetadataIsValid(&live.Models[0]));
  CHECK_ALL(StrView_EqCStr(live.Models[1].Id, "qwen/qwen3-max"),
            live.Models[1].MaxTokens == 0, !live.Models[1].Reasoning,
            live.Models[1].EffortMask == 0,
            Model_MetadataIsValid(&live.Models[1]));
  CHECK_ALL(StrView_EqCStr(live.Models[2].Id, "mistralai/mistral-medium"),
            StrView_EqCStr(live.Models[2].Name, "Mistral Medium"),
            live.Models[2].ContextWindow == 131072,
            !live.Models[2].Reasoning);
  ArenaRelease(&arena);
}

static void TestCodexModelDiscovery() {
  Arena arena = ArenaCreate();
  const ProviderSpec* spec = ProviderRegistry_Find(SVL("openai-codex"));

  // The backend scopes every request to a ChatGPT account, so a token without
  // one never reaches the network.
  ModelsFetchFixture unused = {};
  ProviderAuth noAccount = {SVL("access"), {}};
  ProviderModelList refused = Responses_ListModelsWithFetch(
      &arena, spec->Endpoint, noAccount, nullptr, FetchModelsFixture, &unused);
  CHECK_ALL(refused.Status == ProviderModelListStatus::Failed, unused.Calls == 0);

  ModelsFetchFixture fixture = {};
  fixture.ResponseCount = 1;
  fixture.Responses[0].Status = 200;
  fixture.Responses[0].Body = SVL(
      "{\"models\":["
      "{\"slug\":\"gpt-mini\",\"display_name\":\"GPT Mini\",\"visibility\":\"list\","
      "\"priority\":5,\"context_window\":200000,"
      "\"supported_reasoning_levels\":[{\"effort\":\"minimal\"},{\"effort\":\"low\"}],"
      "\"default_reasoning_level\":\"minimal\"},"
      "{\"slug\":\"internal\",\"visibility\":\"hide\",\"priority\":0},"
      "{\"slug\":\"gpt-6.1-sol\",\"display_name\":\"GPT-6.1 Sol\",\"visibility\":\"list\","
      "\"priority\":1,\"context_window\":400000,"
      "\"supported_reasoning_levels\":[{\"effort\":\"low\"},{\"effort\":\"medium\"},"
      "{\"effort\":\"high\"},{\"effort\":\"xhigh\"}],"
      "\"default_reasoning_level\":\"medium\"},"
      "{\"slug\":\"gpt-plain\",\"visibility\":\"list\",\"priority\":5}"
      "]}");
  ProviderAuth auth = {SVL("access"), SVL("acct-1")};
  ProviderModelList live = Responses_ListModelsWithFetch(
      &arena, spec->Endpoint, auth, nullptr, FetchModelsFixture, &fixture);
  CHECK_ALL(live.Status == ProviderModelListStatus::Success, live.Count == 3,
            fixture.RequestsValid,
            StrView_EqCStr(fixture.Urls[0],
                           "https://chatgpt.com/backend-api/codex/models?client_version="
                           RESPONSES_CODEX_CLIENT_VERSION),
            StrView_EqCStr(fixture.Authorizations[0], "Bearer access"),
            StrView_EqCStr(fixture.AccountIds[0], "acct-1"),
            StrView_EqCStr(fixture.Originators[0], "clanker"));

  // Hidden models are dropped and the rest follow the backend's priority,
  // keeping its order among ties.
  CHECK_ALL(StrView_EqCStr(live.Models[0].Id, "gpt-6.1-sol"),
            StrView_EqCStr(live.Models[0].Name, "GPT-6.1 Sol"),
            StrView_EqCStr(live.Models[0].Provider, "openai-codex"),
            live.Models[0].ContextWindow == 400000, live.Models[0].Reasoning,
            Model_SupportsEffort(&live.Models[0], ModelEffort::XHigh),
            !Model_SupportsEffort(&live.Models[0], ModelEffort::Max),
            live.Models[0].DefaultEffort == ModelEffort::Medium,
            Model_MetadataIsValid(&live.Models[0]));
  // Levels Clanker has no tier for are not offered, nor defaulted to.
  CHECK_ALL(StrView_EqCStr(live.Models[1].Id, "gpt-mini"),
            live.Models[1].EffortMask == ModelEffort_Bit(ModelEffort::Low),
            live.Models[1].DefaultEffort == ModelEffort::None);
  CHECK_ALL(StrView_EqCStr(live.Models[2].Id, "gpt-plain"),
            StrView_EqCStr(live.Models[2].Name, "gpt-plain"),
            !live.Models[2].Reasoning);
  ArenaRelease(&arena);
}

static void RemoveDir(const char* path) {
#if defined(_WIN32)
  _rmdir(path);
#else
  rmdir(path);
#endif
}

static void TestModelCache() {
  Arena arena = ArenaCreate();
  const ProviderSpec* codex = ProviderRegistry_Find(SVL("openai-codex"));
  StrView root = SVL("clanker_model_cache_test");
  const char* cacheFile = "clanker_model_cache_test/models/openai-codex.json";
  remove(cacheFile);

  Model bare = ModelCache_CreateModel(&arena, root, codex, SVL("gpt-6.1-sol"));
  CHECK_ALL(bare.ContextWindow == 0, bare.EffortMask == 0,
            !ModelCache_Save(StrView{}, codex->Id, &bare, 1));

  Model listed[2] = {codex->CreateModel(&arena, codex, SVL("gpt-6.1-sol")),
                     codex->CreateModel(&arena, codex, SVL("gpt-tiered"))};
  listed[0].Name = SVL("GPT-6.1 Sol");
  listed[0].ContextWindow = 400000;
  listed[0].Reasoning = true;
  listed[0].EffortMask = MODEL_EFFORT_STANDARD_MASK;
  listed[0].DefaultEffort = ModelEffort::Medium;
  listed[1].ContextWindow = 200000;
  listed[1].ContextWindowOptions[0] = 200000;
  listed[1].ContextWindowOptions[1] = 1000000;
  listed[1].ContextWindowOptionCount = 2;
  CHECK(ModelCache_Save(root, codex->Id, listed, 2));

  // A model created without a catalog fetch -- at startup, say -- carries what
  // the last listing said; identity and endpoint still come from the registry.
  Model sol = ModelCache_CreateModel(&arena, root, codex, SVL("gpt-6.1-sol"));
  CHECK_ALL(StrView_EqCStr(sol.Name, "GPT-6.1 Sol"), sol.ContextWindow == 400000,
            sol.Reasoning, sol.EffortMask == MODEL_EFFORT_STANDARD_MASK,
            sol.DefaultEffort == ModelEffort::Medium,
            StrView_EqCStr(sol.Provider, "openai-codex"),
            StrView_EqCStr(sol.BaseUrl, "https://chatgpt.com/backend-api"));
  Model tiered = ModelCache_CreateModel(&arena, root, codex, SVL("gpt-tiered"));
  CHECK_ALL(StrView_EqCStr(tiered.Name, "gpt-tiered"),
            tiered.ContextWindow == 200000, tiered.ContextWindowOptionCount == 2,
            tiered.ContextWindowOptions[1] == 1000000, tiered.EffortMask == 0);
  Model unlisted = ModelCache_CreateModel(&arena, root, codex, SVL("gpt-unlisted"));
  CHECK(unlisted.ContextWindow == 0);

  // Effort names from a newer build are skipped; inconsistent entries are not
  // applied at all.
  CHECK(ConfigFile_WriteAtomic(
      &arena, SV(cacheFile),
      SVL("{\"models\":["
          "{\"id\":\"future\",\"contextWindow\":1000,\"efforts\":[\"high\",\"galactic\"]},"
          "{\"id\":\"broken\",\"contextWindow\":1000,\"efforts\":[\"low\"],"
          "\"defaultEffort\":\"high\"}]}")));
  Model future = ModelCache_CreateModel(&arena, root, codex, SVL("future"));
  CHECK_ALL(future.ContextWindow == 1000,
            future.EffortMask == ModelEffort_Bit(ModelEffort::High));
  Model broken = ModelCache_CreateModel(&arena, root, codex, SVL("broken"));
  CHECK_ALL(broken.ContextWindow == 0, broken.EffortMask == 0,
            broken.DefaultEffort == ModelEffort::None);

  remove(cacheFile);
  RemoveDir("clanker_model_cache_test/models");
  RemoveDir("clanker_model_cache_test");
  ArenaRelease(&arena);
}

void TestProviderRegistry() {
  CHECK_ALL(ProviderRegistry_Count() == 2,
            ProviderRegistry_At(ProviderRegistry_Count()) == nullptr,
            ProviderRegistry_Find(SVL("nonexistent")) == nullptr,
            ProviderRegistry_Find(SVL("missing")) == nullptr);
  CHECK_ALL(ProviderRegistry_Find(SVL("openai")) == nullptr,
            ProviderRegistry_Find(SVL("anthropic")) == nullptr,
            ProviderRegistry_Find(SVL("openai-compat")) == nullptr);

  // The provider picker enumerates the table directly instead of filtering it,
  // so every row must be complete and selectable.
  for (u32 i = 0; i < ProviderRegistry_Count(); i++) {
    const ProviderSpec* spec = ProviderRegistry_At(i);
    CHECK_ALL(spec != nullptr, spec->Id.Length > 0, spec->Name.Length > 0,
              spec->DefaultModel.Length > 0, spec->Endpoint != nullptr,
              spec->CreateProvider != nullptr, spec->CreateModel != nullptr,
              spec->ListModels != nullptr,
              ProviderRegistry_Find(spec->Id) == spec);
  }

  {
    Arena labels = ArenaCreate();
    CHECK(ProviderRegistry_Label(&labels, nullptr).Length == 0);
    StrView label =
        ProviderRegistry_Label(&labels, ProviderRegistry_Find(SVL("openrouter")));
    CHECK(StrView_EqCStr(label, "OpenRouter (openrouter)"));
    ArenaRelease(&labels);
  }

  Arena arena = ArenaCreate();
  const ProviderSpec* openrouter = ProviderRegistry_Find(SVL("openrouter"));
  CHECK(openrouter == ProviderRegistry_At(0));
  CHECK_ALL(StrView_EqCStr(openrouter->Name, "OpenRouter"),
            StrView_EqCStr(openrouter->DefaultModel, "openrouter/auto"),
            strcmp(openrouter->ApiKeyEnvVar, "OPENROUTER_API_KEY") == 0,
            openrouter->CreateProvider != nullptr,
            openrouter->CreateModel != nullptr,
            openrouter->ListModels != nullptr,
            openrouter->Endpoint != nullptr);

  char manualId[] = "vendor/manual";
  Model manual =
      openrouter->CreateModel(&arena, openrouter, StrView{manualId, 13});
  manualId[0] = 'x';
  CHECK_ALL(StrView_EqCStr(manual.Id, "vendor/manual"),
            StrView_EqCStr(manual.Name, "vendor/manual"),
            StrView_Eq(manual.Provider, openrouter->Id),
            StrView_EqCStr(manual.BaseUrl, "https://openrouter.ai/api/v1"),
            manual.ContextWindow == 0, manual.EffortMask == 0);

  // The bootstrap route carries no compiled capabilities; discovery fills
  // them from OpenRouter before the picker displays the catalog.
  Model automatic =
      openrouter->CreateModel(&arena, openrouter, openrouter->DefaultModel);
  CHECK_ALL(StrView_EqCStr(automatic.Id, "openrouter/auto"),
            automatic.ContextWindow == 0, automatic.MaxTokens == 0,
            !automatic.Reasoning, automatic.EffortMask == 0,
            Model_MetadataIsValid(&automatic));

  Provider provider = openrouter->CreateProvider(&arena, openrouter);
  CHECK_ALL(provider.Stream != nullptr,
            provider.Ctx == (void*)openrouter->Endpoint);

  // ChatGPT sign-in replaces an API key, so there is no variable to name.
  const ProviderSpec* codex = ProviderRegistry_Find(SVL("openai-codex"));
  CHECK(codex == ProviderRegistry_At(1));
  CHECK_ALL(StrView_EqCStr(codex->Name, "ChatGPT"),
            StrView_EqCStr(codex->DefaultModel, "gpt-6.1-sol"),
            codex->ApiKeyEnvVar == nullptr,
            codex->SignIn == ProviderSignIn::ChatGpt,
            openrouter->SignIn == ProviderSignIn::OpenRouterKey,
            codex->Endpoint->Dialect == ResponsesDialect::ChatGptCodex,
            openrouter->Endpoint->Dialect == ResponsesDialect::OpenRouter);
  Model codexModel = codex->CreateModel(&arena, codex, codex->DefaultModel);
  CHECK_ALL(StrView_EqCStr(codexModel.Provider, "openai-codex"),
            StrView_EqCStr(codexModel.BaseUrl, "https://chatgpt.com/backend-api"));

  ArenaRelease(&arena);
  TestModelDiscovery();
  TestCodexModelDiscovery();
  TestModelDiscoveryFailures();
  TestModelCache();
}
