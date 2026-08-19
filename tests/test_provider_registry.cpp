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
      "{\"id\":\"o3-pro\",\"name\":\"o3 Pro\",\"context_length\":262144}"
      "]}");
  volatile i32 abort = 0;
  ProviderAuth auth = {SVL("test-key")};

  ProviderModelList live = Responses_ListModelsWithFetch(
      &arena, spec->Endpoint, auth, &abort, FetchModelsFixture, &fixture);
  CHECK_ALL(live.Status == ProviderModelListStatus::Success, live.Count == 2,
            fixture.RequestsValid, fixture.Calls == 1, fixture.AbortSeen == &abort);
  StrBuf expectedAuth = StrBuf_Make(&arena, auth.Token.Length + 8);
  StrBuf_AppendCStr(&expectedAuth, "Bearer ");
  StrBuf_Append(&expectedAuth, auth.Token);
  CHECK_ALL(StrView_EqCStr(fixture.Urls[0], "https://api.openai.com/v1/models"),
            StrView_Eq(fixture.Authorizations[0], StrBuf_View(&expectedAuth)),
            StrView_EqCStr(fixture.Accepts[0], "application/json"));

  // A live ID matching a known model inherits that model's metadata; an unknown
  // one keeps only what the catalog published.
  CHECK_ALL(StrView_EqCStr(live.Models[0].Id, "gpt-5.6-sol"),
            StrView_EqCStr(live.Models[0].Name, "GPT-5.6-Sol"),
            live.Models[0].ContextWindow == 272000,
            Model_SupportsEffort(&live.Models[0], ModelEffort::High),
            Model_SupportsEffort(&live.Models[0], ModelEffort::Ultra));
  CHECK_ALL(StrView_EqCStr(live.Models[1].Id, "o3-pro"),
            StrView_EqCStr(live.Models[1].Name, "o3 Pro"),
            live.Models[1].ContextWindow == 262144,
            live.Models[1].EffortMask == 0);

  ProviderModelList merged = ProviderModelList_Merge(
      &arena, live, spec->FallbackModels, spec->FallbackModelCount);
  CHECK_ALL(merged.Count == 6, StrView_EqCStr(merged.Models[0].Id, "gpt-5.6-sol"),
            StrView_EqCStr(merged.Models[2].Id, "gpt-5.6-terra"));
  ArenaRelease(&arena);
}

static void TestModelDiscoveryFailures() {
  Arena arena = ArenaCreate();
  const ResponsesEndpoint* endpoint = ProviderRegistry_Find(SVL("openai"))->Endpoint;
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

void TestProviderRegistry() {
  CHECK_ALL(ProviderRegistry_Count() == 1,
            ProviderRegistry_At(ProviderRegistry_Count()) == nullptr,
            ProviderRegistry_Find(SVL("nonexistent")) == nullptr,
            ProviderRegistry_Find(SVL("missing")) == nullptr);
  // The endpoints that only spoke chat-completions are gone, not hidden.
  CHECK_ALL(ProviderRegistry_Find(SVL("anthropic")) == nullptr,
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
        ProviderRegistry_Label(&labels, ProviderRegistry_Find(SVL("openai")));
    CHECK(StrView_EqCStr(label, "OpenAI (openai)"));
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

  // The Responses API accepts the full effort range, so the rows advertise it.
  Model openaiDefault = openai->CreateModel(&arena, openai, openai->DefaultModel);
  CHECK_ALL(StrView_EqCStr(openaiDefault.Name, "GPT-5.6-Sol"),
            openaiDefault.ContextWindow == 272000,
            Model_SupportsEffort(&openaiDefault, ModelEffort::Low),
            Model_SupportsEffort(&openaiDefault, ModelEffort::High),
            Model_SupportsEffort(&openaiDefault, ModelEffort::Ultra),
            Model_MetadataIsValid(&openaiDefault));

  Model luna = openai->CreateModel(&arena, openai, SVL("gpt-5.6-luna"));
  CHECK_ALL(Model_SupportsEffort(&luna, ModelEffort::Max),
            !Model_SupportsEffort(&luna, ModelEffort::Ultra),
            Model_MetadataIsValid(&luna));

  Provider openaiProvider = openai->CreateProvider(&arena, openai);
  CHECK_ALL(openaiProvider.Stream != nullptr,
            openaiProvider.Ctx == (void*)openai->Endpoint);

  ArenaRelease(&arena);
  TestModelDiscovery();
  TestModelDiscoveryFailures();
}
