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

// Absolute path of one session's durable JSONL log, so a caller can report
// where the transcript lives without re-deriving the project key. The result is
// owned by `arena`; an uninitialized repository or invalid key yields an empty
// view. The file itself need not exist yet.
StrView JsonlRepository_SessionPath(Arena* arena, JsonlRepository* repository,
                                    StrView projectKey, DurableId sessionId);

// Directory for one session's non-transcript artifacts, currently the `shell`
// tool's output-overflow logs. Sits beside the JSONL log and is named for the
// session, so it is discarded with the session rather than accumulating in a
// shared location. Owned by `arena`; not created here, and empty for an
// uninitialized repository or invalid key.
StrView JsonlRepository_SessionDir(Arena* arena, JsonlRepository* repository,
                                   StrView projectKey, DurableId sessionId);
