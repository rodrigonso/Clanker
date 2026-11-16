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

// How `/auth` signs in to a provider. OpenRouter's OAuth mints a long-lived API
// key; ChatGPT sign-in issues refreshable tokens for a ChatGPT subscription.
enum class ProviderSignIn : u8 { OpenRouterKey, ChatGpt };

struct ProviderSpec {
  StrView Id;
  StrView Name;
  StrView DefaultModel;
  const char* ApiKeyEnvVar;  // null when the provider takes no API key
  ProviderSignIn SignIn;
  // Every provider speaks the Responses protocol; the endpoint carries its root
  // and dialect.
  const ResponsesEndpoint* Endpoint;
  ProviderFactoryFn CreateProvider;
  ProviderModelFactoryFn CreateModel;
  ProviderListModelsFn ListModels;
};

// Stable process-lifetime registry entries. At returns nullptr out of range.
u32 ProviderRegistry_Count();
const ProviderSpec* ProviderRegistry_At(u32 index);
const ProviderSpec* ProviderRegistry_Find(StrView providerId);

// "Name (id)", allocated from `arena`.
StrView ProviderRegistry_Label(Arena* arena, const ProviderSpec* spec);
