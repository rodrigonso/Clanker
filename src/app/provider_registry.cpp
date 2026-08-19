#include "app/provider_registry.h"

// Every provider here speaks the same protocol; only the endpoint data differs.
// Adding a vendor is adding a row, not a wire implementation.

namespace {

// ---- OpenAI --------------------------------------------------------------
// Effort is advertised as the portable low/medium/high triple because that is
// what `reasoning_effort` accepts across compatible servers.
const Model OPENAI_MODELS[] = {
    {
        SVL("gpt-5.6-sol"),
        SVL("GPT-5.6-Sol"),
        SVL("openai"),
        SVL("https://api.openai.com"),
        272000,
        0,
        true,
        MODEL_EFFORT_STANDARD_MASK,
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
        MODEL_EFFORT_STANDARD_MASK,
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
        MODEL_EFFORT_STANDARD_MASK,
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
        MODEL_EFFORT_STANDARD_MASK,
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
        MODEL_EFFORT_STANDARD_MASK,
        ModelEffort::Medium,
    },
};

// ---- Anthropic -----------------------------------------------------------
// Reached through Anthropic's OpenAI-compatibility endpoint, which exposes
// neither extended thinking nor the 1M-token context beta. Effort is therefore
// advertised as unsupported and the window as the standard 200K: claiming
// either would offer the user a control that silently does nothing.
const Model ANTHROPIC_MODELS[] = {
    {
        SVL("claude-sonnet-4-6"),
        SVL("Claude Sonnet 4.6"),
        SVL("anthropic"),
        SVL("https://api.anthropic.com"),
        200000,
        64000,
    },
    {
        SVL("claude-opus-4-6"),
        SVL("Claude Opus 4.6"),
        SVL("anthropic"),
        SVL("https://api.anthropic.com"),
        200000,
        64000,
    },
    {
        SVL("claude-haiku-4-5"),
        SVL("Claude Haiku 4.5"),
        SVL("anthropic"),
        SVL("https://api.anthropic.com"),
        200000,
        64000,
    },
};

const ChatEndpoint OPENAI_ENDPOINT = {
    SVL("openai"),
    SVL("https://api.openai.com"),
    "OPENAI_BASE_URL",
    OPENAI_MODELS,
    (u32)(sizeof(OPENAI_MODELS) / sizeof(OPENAI_MODELS[0])),
    false,
};

const ChatEndpoint ANTHROPIC_ENDPOINT = {
    SVL("anthropic"),
    SVL("https://api.anthropic.com"),
    "ANTHROPIC_BASE_URL",
    ANTHROPIC_MODELS,
    (u32)(sizeof(ANTHROPIC_MODELS) / sizeof(ANTHROPIC_MODELS[0])),
    false,
};

// The escape hatch: any other OpenAI-compatible server. Defaults to Ollama's
// local address, which needs no credential, so `clank -p ...` against a local
// model works with nothing configured at all.
const ChatEndpoint COMPAT_ENDPOINT = {
    SVL("openai-compat"),
    SVL("http://localhost:11434"),
    "OPENAI_COMPAT_BASE_URL",
    nullptr,
    0,
    true,
};

Provider CreateChatProvider(Arena* arena, const ProviderSpec* spec) {
  (void)arena;
  Provider provider = ChatCompletions_Provider();
  // Process-lifetime static; the harness copies the Provider, not the endpoint.
  provider.Ctx = (void*)(spec ? spec->Endpoint : nullptr);
  return provider;
}

Model CreateChatModel(Arena* arena, const ProviderSpec* spec, StrView modelId) {
  return ChatCompletions_Model(arena, spec ? spec->Endpoint : nullptr,
                               StrView_Dup(arena, modelId));
}

ProviderModelList ListChatModels(Arena* arena, const ProviderSpec* spec,
                                 ProviderAuth auth, volatile i32* abort) {
  return ChatCompletions_ListModels(arena, spec ? spec->Endpoint : nullptr,
                                    auth, abort);
}

const ProviderSpec PROVIDERS[] = {
    {
        SVL("openai"),
        SVL("OpenAI"),
        SVL("gpt-5.6-sol"),
        "OPENAI_API_KEY",
        {},
        &OPENAI_ENDPOINT,
        CreateChatProvider,
        CreateChatModel,
        ListChatModels,
        OPENAI_MODELS,
        (u32)(sizeof(OPENAI_MODELS) / sizeof(OPENAI_MODELS[0])),
    },
    {
        SVL("anthropic"),
        SVL("Anthropic"),
        SVL("claude-sonnet-4-6"),
        "ANTHROPIC_API_KEY",
        SVL("compatibility endpoint: no extended thinking or 1M context"),
        &ANTHROPIC_ENDPOINT,
        CreateChatProvider,
        CreateChatModel,
        ListChatModels,
        ANTHROPIC_MODELS,
        (u32)(sizeof(ANTHROPIC_MODELS) / sizeof(ANTHROPIC_MODELS[0])),
    },
    {
        SVL("openai-compat"),
        SVL("OpenAI-compatible"),
        SVL("qwen3-coder"),
        "OPENAI_COMPAT_API_KEY",
        SVL("any OpenAI-compatible server; defaults to local Ollama"),
        &COMPAT_ENDPOINT,
        CreateChatProvider,
        CreateChatModel,
        ListChatModels,
        nullptr,
        0,
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
  StrBuf label = StrBuf_Make(
      arena, spec->Name.Length + spec->Id.Length + spec->Note.Length + 16);
  StrBuf_Append(&label, spec->Name);
  StrBuf_AppendCStr(&label, " (");
  StrBuf_Append(&label, spec->Id);
  StrBuf_AppendChar(&label, ')');
  if (spec->Note.Length) {
    StrBuf_AppendCStr(&label, " -- ");
    StrBuf_Append(&label, spec->Note);
  }
  return StrBuf_View(&label);
}
