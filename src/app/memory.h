#pragma once

#include "agent/harness.h"
#include "app/extensions.h"
#include "app/wiki.h"

// The memory wiki as a session extension. It keeps the live repository's wiki
// writable as freely as the working directory, protects the journal and
// dreaming state from the model, checks each page the model writes, and
// records which sessions and pages dreaming should revisit. The prompt side
// lives in PromptContext; both resolve the same location from the working
// directory, so neither depends on the other running first.
struct MemoryExtension {
  AgentHarness* Harness;
  StrView Store;
  bool Enabled;
  // Holds everything below; reset on every session change.
  Arena Mem;
  WikiLocation Location;
  // Canonical, for comparing tool targets.
  StrView Root;
  StrView Pages;
  StrView WorkingDirectory;
  // The harness keeps a view of this array as its writable roots.
  StrView WritableRoots[1];
  WikiJournalEntry Session;
  // When the live session was last journaled; 0 before its first prompt.
  u64 JournaledMs;
};

// A session in use is journaled again after this long, so dreaming keeps
// rechecking one that outlives its first journal entry.
constexpr u64 MEMORY_REJOURNAL_MS = 24ull * 60 * 60 * 1000;

// `store` must outlive the extension.
void MemoryExtension_Init(MemoryExtension* memory, AgentHarness* harness,
                          StrView store, bool enabled);
AppExtension MemoryExtension_Make(MemoryExtension* memory);
// Turns the extension on or off for later tool calls without re-registering it.
void MemoryExtension_SetEnabled(MemoryExtension* memory, bool enabled);
