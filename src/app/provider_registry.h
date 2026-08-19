#pragma once
#include "base/arena.h"
#include "base/basics.h"
#include "base/str.h"
#include "llm/model_list.h"
#include "llm/responses.h"
#include "llm/types.h"

struct ProviderSpec;

// Provider factories may allocate provider state from `arena`. Model factories
// duplicate caller-owned IDs into `arena`. Live-list results and their strings
// are also owned by `arena`. Each takes its own spec so one implementation can
// back every registry entry -- entries differ only by endpoint data.
typedef Provider (*ProviderFactoryFn)(Arena* arena, const ProviderSpec* spec);
typedef Model (*ProviderModelFactoryFn)(Arena* arena, const ProviderSpec* spec,
                                        StrView modelId);
typedef ProviderModelList (*ProviderListModelsFn)(Arena* arena,
                                                  const ProviderSpec* spec,
                                                  ProviderAuth auth,
                                                  volatile i32* abort);

struct ProviderSpec {
  StrView Id;
  StrView Name;
  StrView DefaultModel;
  const char* ApiKeyEnvVar;
  // The Responses endpoint this provider talks to. Every provider has one,
  // since the Responses API is the only protocol this build speaks.
  const ResponsesEndpoint* Endpoint;
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

// "Name (id)", allocated from `arena`.
StrView ProviderRegistry_Label(Arena* arena, const ProviderSpec* spec);
