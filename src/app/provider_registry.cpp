#include "app/provider_registry.h"

// OpenRouter is the default provider; ChatGPT serves a ChatGPT subscription
// through the Codex backend. Each live catalog owns model availability and
// capabilities; this registry contains no model table.

namespace {

const ResponsesEndpoint OPENROUTER_ENDPOINT = {
    SVL("openrouter"),
    SVL("https://openrouter.ai/api/v1"),
    "OPENROUTER_BASE_URL",
    ResponsesDialect::OpenRouter,
};

const ResponsesEndpoint CODEX_ENDPOINT = {
    SVL("openai-codex"),
    SVL("https://chatgpt.com/backend-api"),
    "OPENAI_CODEX_BASE_URL",
    ResponsesDialect::ChatGptCodex,
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

// OpenRouter stays first: it is the default when nothing else is configured.
const ProviderSpec PROVIDERS[] = {
    {
        SVL("openrouter"),
        SVL("OpenRouter"),
        SVL("openrouter/auto"),
        "OPENROUTER_API_KEY",
        ProviderSignIn::OpenRouterKey,
        &OPENROUTER_ENDPOINT,
        CreateResponsesProvider,
        CreateResponsesModel,
        ListResponsesModels,
    },
    {
        SVL("openai-codex"),
        SVL("ChatGPT"),
        SVL("gpt-6.1-sol"),
        nullptr,
        ProviderSignIn::ChatGpt,
        &CODEX_ENDPOINT,
        CreateResponsesProvider,
        CreateResponsesModel,
        ListResponsesModels,
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
