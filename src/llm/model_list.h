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

inline ProviderModelList ProviderModelList_Error(Arena* arena,
                                                 ProviderModelListStatus status,
                                                 StrView error) {
  ProviderModelList result = {};
  result.Status = status;
  result.Error = StrView_Dup(arena, error);
  return result;
}

inline Model Model_Dup(Arena* arena, const Model& source) {
  Model result = source;
  result.Id = StrView_Dup(arena, source.Id);
  result.Name = StrView_Dup(arena, source.Name);
  result.Provider = StrView_Dup(arena, source.Provider);
  result.BaseUrl = StrView_Dup(arena, source.BaseUrl);
  return result;
}
