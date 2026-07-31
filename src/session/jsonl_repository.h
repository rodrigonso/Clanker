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
  // Set once, during Init: true when ~/.clank/sessions/v1 has session data
  // but this v2 store does not yet have any of its own. v2 is an
  // intentionally incompatible schema -- v1 sessions are never migrated,
  // read, listed, or opened here -- so a caller (e.g. app startup) can use
  // this flag to show a one-time informational notice that old sessions
  // were left behind rather than silently going missing.
  bool HasUnmigratedV1;
};

void JsonlRepository_Init(JsonlRepository* repository, StrView dataRoot);
void JsonlRepository_Destroy(JsonlRepository* repository);
SessionRepository JsonlRepository_AsRepository(JsonlRepository* repository);
