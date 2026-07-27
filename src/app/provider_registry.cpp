#include "app/provider_registry.h"

#include "llm/anthropic.h"

static const Model ANTHROPIC_FALLBACK_MODELS[] = {
    {
        SVL("claude-sonnet-4-6"),
        SVL("Claude Sonnet 4.6"),
        SVL("anthropic"),
        SVL("anthropic-messages"),
        SVL("https://api.anthropic.com"),
        200000,
        8192,
        false,
    },
    {
        SVL("claude-opus-4-6"),
        SVL("Claude Opus 4.6"),
        SVL("anthropic"),
        SVL("anthropic-messages"),
        SVL("https://api.anthropic.com"),
        200000,
        8192,
        false,
    },
    {
        SVL("claude-haiku-4-5"),
        SVL("Claude Haiku 4.5"),
        SVL("anthropic"),
        SVL("anthropic-messages"),
        SVL("https://api.anthropic.com"),
        200000,
        8192,
        false,
    },
};

static Provider AnthropicProviderFactory(Arena* arena) {
  (void)arena;
  return Anthropic_Provider();
}

static Model AnthropicModelFactory(Arena* arena, StrView modelId) {
  Model model = Anthropic_Model(StrView_Dup(arena, modelId));
  for (u32 i = 0;
       i < sizeof(ANTHROPIC_FALLBACK_MODELS) / sizeof(ANTHROPIC_FALLBACK_MODELS[0]); i++) {
    if (StrView_Eq(model.Id, ANTHROPIC_FALLBACK_MODELS[i].Id)) {
      model.Name = ANTHROPIC_FALLBACK_MODELS[i].Name;
      break;
    }
  }
  return model;
}

static const ProviderSpec PROVIDERS[] = {
    {
        SVL("anthropic"),
        SVL("Anthropic"),
        SVL("claude-sonnet-4-6"),
        "ANTHROPIC_API_KEY",
        AnthropicProviderFactory,
        AnthropicModelFactory,
        Anthropic_ListModels,
        ANTHROPIC_FALLBACK_MODELS,
        (u32)(sizeof(ANTHROPIC_FALLBACK_MODELS) / sizeof(ANTHROPIC_FALLBACK_MODELS[0])),
    },
};

u32 ProviderRegistry_Count() { return (u32)(sizeof(PROVIDERS) / sizeof(PROVIDERS[0])); }

const ProviderSpec* ProviderRegistry_At(u32 index) {
  return index < ProviderRegistry_Count() ? &PROVIDERS[index] : nullptr;
}

const ProviderSpec* ProviderRegistry_Find(StrView providerId) {
  for (u32 i = 0; i < ProviderRegistry_Count(); i++) {
    if (StrView_Eq(PROVIDERS[i].Id, providerId)) return &PROVIDERS[i];
  }
  return nullptr;
}
