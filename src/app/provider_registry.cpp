#include "app/provider_registry.h"

// Every provider here speaks the same protocol; only the endpoint data differs.
// Adding a vendor is adding a row, not a wire implementation -- provided that
// vendor serves the Responses API, which is the only protocol this build
// speaks.

namespace {

// ---- OpenAI --------------------------------------------------------------
// Effort tiers are advertised per model because the Responses API accepts the
// full range and the models differ in how far up it they go.
const Model OPENAI_MODELS[] = {
    {
        SVL("gpt-5.6-sol"),
        SVL("GPT-5.6-Sol"),
        SVL("openai"),
        SVL("https://api.openai.com"),
        272000,
        0,
        true,
        MODEL_EFFORT_ALL_MASK,
        ModelEffort::Low,
    },
    {
        SVL("gpt-5.6-terra"),
        SVL("GPT-5.6-Terra"),
        SVL("openai"),
        SVL("https://api.openai.com"),
        272000,
        0,
        true,
        MODEL_EFFORT_ALL_MASK,
        ModelEffort::Medium,
    },
    {
        SVL("gpt-5.6-luna"),
        SVL("GPT-5.6-Luna"),
        SVL("openai"),
        SVL("https://api.openai.com"),
        272000,
        0,
        true,
        MODEL_EFFORT_THROUGH_MAX_MASK,
        ModelEffort::Medium,
    },
    {
        SVL("gpt-5.5"),
        SVL("GPT-5.5"),
        SVL("openai"),
        SVL("https://api.openai.com"),
        272000,
        0,
        true,
        MODEL_EFFORT_STANDARD_MASK | ModelEffort_Bit(ModelEffort::XHigh),
        ModelEffort::Medium,
    },
    {
        SVL("gpt-5.2"),
        SVL("GPT-5.2"),
        SVL("openai"),
        SVL("https://api.openai.com"),
        272000,
        0,
        true,
        MODEL_EFFORT_STANDARD_MASK | ModelEffort_Bit(ModelEffort::XHigh),
        ModelEffort::Medium,
    },
};

const ResponsesEndpoint OPENAI_ENDPOINT = {
    SVL("openai"),
    SVL("https://api.openai.com"),
    "OPENAI_BASE_URL",
    OPENAI_MODELS,
    (u32)(sizeof(OPENAI_MODELS) / sizeof(OPENAI_MODELS[0])),
};

Provider CreateResponsesProvider(Arena* arena, const ProviderSpec* spec) {
  (void)arena;
  Provider provider = Responses_Provider();
  // Process-lifetime static; the harness copies the Provider, not the endpoint.
  provider.Ctx = (void*)(spec ? spec->Endpoint : nullptr);
  return provider;
}

Model CreateResponsesModel(Arena* arena, const ProviderSpec* spec,
                           StrView modelId) {
  return Responses_Model(arena, spec ? spec->Endpoint : nullptr,
                         StrView_Dup(arena, modelId));
}

ProviderModelList ListResponsesModels(Arena* arena, const ProviderSpec* spec,
                                      ProviderAuth auth, volatile i32* abort) {
  return Responses_ListModels(arena, spec ? spec->Endpoint : nullptr, auth,
                              abort);
}

const ProviderSpec PROVIDERS[] = {
    {
        SVL("openai"),
        SVL("OpenAI"),
        SVL("gpt-5.6-sol"),
        "OPENAI_API_KEY",
        &OPENAI_ENDPOINT,
        CreateResponsesProvider,
        CreateResponsesModel,
        ListResponsesModels,
        OPENAI_MODELS,
        (u32)(sizeof(OPENAI_MODELS) / sizeof(OPENAI_MODELS[0])),
    },
};

}  // namespace

u32 ProviderRegistry_Count() {
  return (u32)(sizeof(PROVIDERS) / sizeof(PROVIDERS[0]));
}

const ProviderSpec* ProviderRegistry_At(u32 index) {
  return index < ProviderRegistry_Count() ? &PROVIDERS[index] : nullptr;
}

const ProviderSpec* ProviderRegistry_Find(StrView providerId) {
  for (u32 i = 0; i < ProviderRegistry_Count(); i++) {
    if (StrView_Eq(PROVIDERS[i].Id, providerId)) return &PROVIDERS[i];
  }
  return nullptr;
}

StrView ProviderRegistry_Label(Arena* arena, const ProviderSpec* spec) {
  if (!spec) return StrView{};
  StrBuf label =
      StrBuf_Make(arena, spec->Name.Length + spec->Id.Length + 4);
  StrBuf_Append(&label, spec->Name);
  StrBuf_AppendCStr(&label, " (");
  StrBuf_Append(&label, spec->Id);
  StrBuf_AppendChar(&label, ')');
  return StrBuf_View(&label);
}
