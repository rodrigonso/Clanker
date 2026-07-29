#include "app/provider_registry.h"

#include "llm/anthropic.h"

static Provider AnthropicProviderFactory(Arena* arena) {
  (void)arena;
  return Anthropic_Provider();
}

static Model AnthropicModelFactory(Arena* arena, StrView modelId) {
  return Anthropic_Model(StrView_Dup(arena, modelId));
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
        Anthropic_FallbackModels(),
        Anthropic_FallbackModelCount(),
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
