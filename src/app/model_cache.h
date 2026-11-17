#pragma once
#include "app/provider_registry.h"
#include "base/arena.h"
#include "base/basics.h"
#include "base/str.h"
#include "llm/types.h"

// A model's context window and reasoning-effort tiers come only from its
// provider's live catalog, which is fetched when a picker needs it. The last
// successful listing is kept per provider at
// <agentDirectory>/models/<provider>.json, so a model created at startup or on
// a session switch is as complete as one just picked. An empty
// `agentDirectory` disables the cache.

// Replaces the provider's cached listing. Best-effort: false when not written.
bool ModelCache_Save(StrView agentDirectory, StrView providerId,
                     const Model* models, u32 count);

// `spec->CreateModel`, completed with the cached catalog metadata for
// `modelId` when the cache holds a valid entry for it.
Model ModelCache_CreateModel(Arena* arena, StrView agentDirectory,
                             const ProviderSpec* spec, StrView modelId);
