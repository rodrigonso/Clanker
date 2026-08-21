#include <string.h>

#include "app/provider_registry.h"
#include "base/arena.h"
#include "check.h"
#include "llm/responses.h"

struct ModelsFetchFixture {
  HttpResponse Responses[2];
  u32 ResponseCount;
  u32 Calls;
  StrView Urls[2];
  StrView Authorizations[2];
  StrView Accepts[2];
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
  fixture->AbortSeen = abort;
  return fixture->Responses[call];
}

static void TestModelDiscoveryFailures() {
  Arena arena = ArenaCreate();
  const ResponsesEndpoint* endpoint =
      ProviderRegistry_Find(SVL("openrouter"))->Endpoint;
  ProviderAuth key = {SVL("test-key")};

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
  ProviderAuth auth = {SVL("test-key")};

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

void TestProviderRegistry() {
  CHECK_ALL(ProviderRegistry_Count() == 1,
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

  ArenaRelease(&arena);
  TestModelDiscovery();
  TestModelDiscoveryFailures();
}
