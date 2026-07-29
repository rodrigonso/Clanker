#pragma once
#include "base/arena.h"
#include "base/basics.h"
#include "base/str.h"
#include "llm/types.h"

enum class ProviderModelListStatus : u8 {
  Success,
  Unavailable,
  Failed,
};

struct ProviderModelList {
  ProviderModelListStatus Status;
  Model* Models;
  u32 Count;
  StrView Error;
};

inline Model Model_Dup(Arena* arena, const Model& source) {
  Model result = source;
  result.Id = StrView_Dup(arena, source.Id);
  result.Name = StrView_Dup(arena, source.Name);
  result.Provider = StrView_Dup(arena, source.Provider);
  result.Api = StrView_Dup(arena, source.Api);
  result.BaseUrl = StrView_Dup(arena, source.BaseUrl);
  return result;
}

namespace model_list_detail {

inline void MergeFallbackMetadata(Model* target, const Model& fallback) {
  if (!target->Name.Length) target->Name = fallback.Name;
  if (!target->Provider.Length) target->Provider = fallback.Provider;
  if (!target->Api.Length) target->Api = fallback.Api;
  if (!target->BaseUrl.Length) target->BaseUrl = fallback.BaseUrl;
  if (!target->ContextWindow) target->ContextWindow = fallback.ContextWindow;
  if (!target->MaxTokens) target->MaxTokens = fallback.MaxTokens;
  if (!target->EffortMask) {
    target->EffortMask = fallback.EffortMask;
    target->DefaultEffort = fallback.DefaultEffort;
  }
  if (!target->ContextWindowOptionCount &&
      fallback.ContextWindowOptionCount) {
    target->ContextWindowOptionCount = fallback.ContextWindowOptionCount;
    for (u32 i = 0; i < fallback.ContextWindowOptionCount; i++)
      target->ContextWindowOptions[i] = fallback.ContextWindowOptions[i];
  }
}

}  // namespace model_list_detail

// Keep the first model for each ID. Live entries are considered first, so their
// authoritative metadata wins while fallback entries fill missing capabilities
// and add models absent from discovery.
inline ProviderModelList ProviderModelList_Merge(Arena* arena, const ProviderModelList& live,
                                                 const Model* fallback, u32 fallbackCount) {
  if (live.Status != ProviderModelListStatus::Success) return live;

  ProviderModelList result = {};
  result.Status = ProviderModelListStatus::Success;
  u32 capacity = live.Count + fallbackCount;
  result.Models = capacity ? ArenaPushArray(arena, Model, capacity) : nullptr;

  for (u32 sourceIndex = 0; sourceIndex < 2; sourceIndex++) {
    const Model* models = sourceIndex == 0 ? live.Models : fallback;
    u32 count = sourceIndex == 0 ? live.Count : fallbackCount;
    for (u32 i = 0; i < count; i++) {
      bool duplicate = false;
      for (u32 k = 0; k < result.Count; k++) {
        if (StrView_Eq(result.Models[k].Id, models[i].Id)) {
          if (sourceIndex == 1) {
            model_list_detail::MergeFallbackMetadata(
                &result.Models[k], models[i]);
          }
          duplicate = true;
          break;
        }
      }
      if (!duplicate) result.Models[result.Count++] = Model_Dup(arena, models[i]);
    }
  }
  return result;
}
