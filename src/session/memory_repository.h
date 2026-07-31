#pragma once

#include "base/thread.h"
#include "session/repository.h"

struct MemorySession;

struct MemoryRepository {
  Arena Mem;
  Mutex Lock;
  Array<MemorySession*> Sessions;
  u64 AppendCalls;
  // Counts calls to SessionLog_ReadAll across every log this repository has
  // handed out. Tests use this to prove the harness reads full history only
  // once, at AgentHarness_Init -- never again on a per-turn/snapshot/mutation
  // path (see AgentHarness_Append / AgentHarness_ReduceSnapshot, which fold
  // incrementally instead of reloading).
  u64 ReadAllCalls;
  u64 FailOnAppendCall;
  bool Initialized;
};

void MemoryRepository_Init(MemoryRepository* repository);
void MemoryRepository_Destroy(MemoryRepository* repository);
SessionRepository MemoryRepository_AsRepository(MemoryRepository* repository);

// Injects one storage failure after `successfulCalls` more append calls. Zero
// fails the next append. The affected writable log becomes permanently faulted.
void MemoryRepository_FailAfterAppends(MemoryRepository* repository,
                                       u32 successfulCalls);
void MemoryRepository_ClearFailure(MemoryRepository* repository);
