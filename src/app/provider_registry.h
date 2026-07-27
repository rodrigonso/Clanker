#pragma once
#include "base/arena.h"
#include "base/basics.h"
#include "base/str.h"
#include "llm/model_list.h"
#include "llm/types.h"

// Provider factories may allocate provider state from `arena`. Model factories
// duplicate caller-owned IDs into `arena`. Live-list results and their strings
// are also owned by `arena`.
typedef Provider (*ProviderFactoryFn)(Arena* arena);
typedef Model (*ProviderModelFactoryFn)(Arena* arena, StrView modelId);
typedef ProviderModelList (*ProviderListModelsFn)(Arena* arena, StrView token,
                                                  volatile i32* abort);

struct ProviderSpec {
  StrView Id;
  StrView Name;
  StrView DefaultModel;
  const char* ApiKeyEnvVar;
  ProviderFactoryFn CreateProvider;
  ProviderModelFactoryFn CreateModel;
  ProviderListModelsFn ListModels;
  const Model* FallbackModels;
  u32 FallbackModelCount;
};

// Stable process-lifetime registry entries. At returns nullptr out of range.
u32 ProviderRegistry_Count();
const ProviderSpec* ProviderRegistry_At(u32 index);
const ProviderSpec* ProviderRegistry_Find(StrView providerId);
