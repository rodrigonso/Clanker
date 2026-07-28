#pragma once

#include "base/process_file_lock.h"
#include "base/thread.h"
#include "session/repository.h"

struct JsonlWriterClaim {
  DurableId SessionId;
  StrView ProjectKey;
  bool Active;
};

struct JsonlRepository {
  Arena Mem;
  Mutex Lock;
  StrView Root;
  Array<JsonlWriterClaim> Claims;
  bool Initialized;
};

void JsonlRepository_Init(JsonlRepository* repository, StrView dataRoot);
void JsonlRepository_Destroy(JsonlRepository* repository);
SessionRepository JsonlRepository_AsRepository(JsonlRepository* repository);
