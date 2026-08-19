#include <string.h>

#include "app/provider_registry.h"
#include "base/arena.h"
#include "check.h"
#include "llm/chat_completions.h"

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

static void TestModelDiscovery() {
  Arena arena = ArenaCreate();
  const ProviderSpec* spec = ProviderRegistry_Find(SVL("openai"));
  ModelsFetchFixture fixture = {};
  fixture.ResponseCount = 1;
  fixture.Responses[0].Status = 200;
  fixture.Responses[0].Body = SVL(
      "{\"object\":\"list\",\"data\":["
      "{\"id\":\"gpt-5.6-sol\"},"
      "{\"id\":\"text-embedding-3-large\"},"
      "{\"id\":\"gpt-audio\"},"
      "{\"id\":\"local/qwen3\",\"name\":\"Qwen3 Coder\",\"context_length\":262144}"
      "]}");
  volatile i32 abort = 0;

  ProviderModelList live = ChatCompletions_ListModelsWithFetch(
      &arena, spec->Endpoint,
      ProviderAuth{SVL("sk-test")}, &abort,
      FetchModelsFixture, &fixture);
  CHECK_ALL(live.Status == ProviderModelListStatus::Success, live.Count == 2,
            fixture.RequestsValid, fixture.Calls == 1, fixture.AbortSeen == &abort);
  CHECK_ALL(StrView_EqCStr(fixture.Urls[0], "https://api.openai.com/v1/models"),
            StrView_EqCStr(fixture.Authorizations[0], "Bearer sk-test"),
            StrView_EqCStr(fixture.Accepts[0], "application/json"));

  // A live ID matching a known model inherits that model's metadata; an unknown
  // one keeps only what the catalog published.
  CHECK_ALL(StrView_EqCStr(live.Models[0].Id, "gpt-5.6-sol"),
            StrView_EqCStr(live.Models[0].Name, "GPT-5.6-Sol"),
            live.Models[0].ContextWindow == 272000,
            Model_SupportsEffort(&live.Models[0], ModelEffort::High),
            !Model_SupportsEffort(&live.Models[0], ModelEffort::Max));
  CHECK_ALL(StrView_EqCStr(live.Models[1].Id, "local/qwen3"),
            StrView_EqCStr(live.Models[1].Name, "Qwen3 Coder"),
            live.Models[1].ContextWindow == 262144,
            live.Models[1].EffortMask == 0);

  ProviderModelList merged = ProviderModelList_Merge(
      &arena, live, spec->FallbackModels, spec->FallbackModelCount);
  CHECK_ALL(merged.Count == 6, StrView_EqCStr(merged.Models[0].Id, "gpt-5.6-sol"),
            StrView_EqCStr(merged.Models[2].Id, "gpt-5.6-terra"));
  ArenaRelease(&arena);
}

static void TestAnonymousModelDiscovery() {
  Arena arena = ArenaCreate();
  const ProviderSpec* spec = ProviderRegistry_Find(SVL("openai-compat"));
  ModelsFetchFixture fixture = {};
  fixture.ResponseCount = 1;
  fixture.Responses[0].Status = 200;
  fixture.Responses[0].Body = SVL("{\"data\":[{\"id\":\"qwen3-coder\"}]}");

  ProviderModelList live = ChatCompletions_ListModelsWithFetch(
      &arena, spec->Endpoint, {}, nullptr, FetchModelsFixture, &fixture);
  CHECK_ALL(live.Status == ProviderModelListStatus::Success, live.Count == 1,
            fixture.RequestsValid, fixture.Calls == 1);
  CHECK_ALL(StrView_EqCStr(fixture.Urls[0], "http://localhost:11434/v1/models"),
            fixture.Authorizations[0].Length == 0);
  CHECK(StrView_EqCStr(live.Models[0].Provider, "openai-compat"));
  ArenaRelease(&arena);
}

static void TestModelDiscoveryFailures() {
  Arena arena = ArenaCreate();
  const ChatEndpoint* endpoint = ProviderRegistry_Find(SVL("openai"))->Endpoint;
  ProviderAuth key = {SVL("sk-test")};

  ProviderModelList noEndpoint = ChatCompletions_ListModelsWithFetch(
      &arena, nullptr, key, nullptr, FetchModelsFixture, nullptr);
  CHECK_ALL(noEndpoint.Status == ProviderModelListStatus::Unavailable,
            noEndpoint.Error.Length != 0);

  ProviderModelList anonymous = ChatCompletions_ListModelsWithFetch(
      &arena, endpoint, {}, nullptr, FetchModelsFixture, nullptr);
  CHECK(anonymous.Status == ProviderModelListStatus::Unavailable);

  volatile i32 abort = 1;
  ModelsFetchFixture unused = {};
  ProviderModelList aborted = ChatCompletions_ListModelsWithFetch(
      &arena, endpoint, key, &abort, FetchModelsFixture, &unused);
  CHECK_ALL(aborted.Status == ProviderModelListStatus::Unavailable, unused.Calls == 0);

  ModelsFetchFixture transport = {};
  transport.ResponseCount = 1;
  transport.Responses[0].Status = -1;
  transport.Responses[0].Error = SVL("offline");
  ProviderModelList unavailable = ChatCompletions_ListModelsWithFetch(
      &arena, endpoint, key, nullptr, FetchModelsFixture, &transport);
  CHECK_ALL(unavailable.Status == ProviderModelListStatus::Unavailable,
            StrView_EqCStr(unavailable.Error, "offline"), transport.RequestsValid);

  ModelsFetchFixture http = {};
  http.ResponseCount = 1;
  http.Responses[0].Status = 401;
  ProviderModelList failed = ChatCompletions_ListModelsWithFetch(
      &arena, endpoint, key, nullptr, FetchModelsFixture, &http);
  CHECK_ALL(failed.Status == ProviderModelListStatus::Failed,
            failed.Error.Length != 0, http.RequestsValid);

  ModelsFetchFixture malformed = {};
  malformed.ResponseCount = 1;
  malformed.Responses[0].Status = 200;
  malformed.Responses[0].Body = SVL("{\"object\":\"list\"}");
  ProviderModelList badBody = ChatCompletions_ListModelsWithFetch(
      &arena, endpoint, key, nullptr, FetchModelsFixture, &malformed);
  CHECK_ALL(badBody.Status == ProviderModelListStatus::Failed,
            badBody.Error.Length != 0, malformed.RequestsValid);
  ArenaRelease(&arena);
}

void TestProviderRegistry() {
  CHECK_ALL(ProviderRegistry_Count() == 3,
            ProviderRegistry_At(ProviderRegistry_Count()) == nullptr,
            ProviderRegistry_Find(SVL("nonexistent")) == nullptr,
            ProviderRegistry_Find(SVL("missing")) == nullptr);

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

  // Picker labels: the plain form for rows without caveats, and the qualified
  // form for endpoints that quietly drop capabilities the name implies.
  {
    Arena labels = ArenaCreate();
    CHECK(ProviderRegistry_Label(&labels, nullptr).Length == 0);
    StrView plain =
        ProviderRegistry_Label(&labels, ProviderRegistry_Find(SVL("openai")));
    CHECK(StrView_EqCStr(plain, "OpenAI (openai)"));

    const ProviderSpec* anthropic = ProviderRegistry_Find(SVL("anthropic"));
    StrView qualified = ProviderRegistry_Label(&labels, anthropic);
    CHECK_ALL(anthropic->Note.Length > 0,
              StrView_StartsWith(qualified, SVL("Anthropic (anthropic) -- ")),
              StrView_Contains(qualified, "extended thinking"));

    const ProviderSpec* compat =
        ProviderRegistry_Find(SVL("openai-compat"));
    CHECK(StrView_Contains(ProviderRegistry_Label(&labels, compat),
                           "OpenAI-compatible"));
    ArenaRelease(&labels);
  }

  Arena arena = ArenaCreate();
  const ProviderSpec* openai = ProviderRegistry_Find(SVL("openai"));
  CHECK(openai == ProviderRegistry_At(0));
  CHECK_ALL(StrView_EqCStr(openai->Name, "OpenAI"),
            StrView_EqCStr(openai->DefaultModel, "gpt-5.6-sol"),
            strcmp(openai->ApiKeyEnvVar, "OPENAI_API_KEY") == 0,
            openai->CreateProvider != nullptr, openai->CreateModel != nullptr,
            openai->ListModels != nullptr, openai->FallbackModels != nullptr,
            openai->FallbackModelCount == 5, openai->Endpoint != nullptr);

  char manualId[] = "gpt-manual";
  Model manual = openai->CreateModel(&arena, openai, StrView{manualId, 10});
  manualId[0] = 'x';
  CHECK_ALL(StrView_EqCStr(manual.Id, "gpt-manual"),
            StrView_EqCStr(manual.Name, "gpt-manual"),
            StrView_Eq(manual.Provider, openai->Id),
            StrView_EqCStr(manual.BaseUrl, "https://api.openai.com"),
            manual.ContextWindow == 0, manual.EffortMask == 0);

  Model openaiDefault = openai->CreateModel(&arena, openai, openai->DefaultModel);
  CHECK_ALL(StrView_EqCStr(openaiDefault.Name, "GPT-5.6-Sol"),
            openaiDefault.ContextWindow == 272000,
            Model_SupportsEffort(&openaiDefault, ModelEffort::Low),
            Model_SupportsEffort(&openaiDefault, ModelEffort::High),
            !Model_SupportsEffort(&openaiDefault, ModelEffort::Ultra),
            Model_MetadataIsValid(&openaiDefault));

  Provider openaiProvider = openai->CreateProvider(&arena, openai);
  CHECK_ALL(openaiProvider.Stream != nullptr,
            openaiProvider.Ctx == (void*)openai->Endpoint);

  const ProviderSpec* anthropic = ProviderRegistry_Find(SVL("anthropic"));
  CHECK(anthropic != nullptr);
  CHECK_ALL(StrView_EqCStr(anthropic->Name, "Anthropic"),
            StrView_EqCStr(anthropic->DefaultModel, "claude-sonnet-4-6"),
            strcmp(anthropic->ApiKeyEnvVar, "ANTHROPIC_API_KEY") == 0,
            anthropic->FallbackModelCount == 3);
  // The compatibility endpoint exposes neither extended thinking nor the 1M
  // context beta, so neither may be advertised to the user.
  Model sonnet = anthropic->CreateModel(&arena, anthropic, anthropic->DefaultModel);
  CHECK_ALL(sonnet.ContextWindow == 200000, sonnet.MaxTokens == 64000,
            !sonnet.Reasoning, sonnet.EffortMask == 0,
            sonnet.DefaultEffort == ModelEffort::None,
            Model_MetadataIsValid(&sonnet));

  const ProviderSpec* compat = ProviderRegistry_Find(SVL("openai-compat"));
  CHECK(compat != nullptr);
  CHECK_ALL(compat->FallbackModels == nullptr, compat->FallbackModelCount == 0,
            compat->Endpoint->AllowAnonymous,
            StrView_EqCStr(compat->Endpoint->BaseUrl, "http://localhost:11434"));

  ArenaRelease(&arena);
  TestModelDiscovery();
  TestAnonymousModelDiscovery();
  TestModelDiscoveryFailures();
}
