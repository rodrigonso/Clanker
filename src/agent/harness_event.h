#pragma once

#include "agent/event.h"
#include "agent/hooks.h"
#include "agent/tool.h"
#include "base/thread.h"
#include "session/reducer.h"

struct AgentHarness;

constexpr u32 AGENT_HARNESS_MAX_WATCHERS = 8;
constexpr u32 AGENT_HARNESS_MAX_WATCH_EVENTS = 256;

// Durable events mirror appended session-tree entries (authoritative for
// snapshots/replay). Transient events are watcher-only lifecycle notices for
// the UI/REPL -- agent/turn/tool/retry/queue progress that v2 never persists;
// a restarted process derives everything it needs from the tree instead.
enum class AgentHarnessEventKind : u8 {
  DurableRecord,
  TransientAgentStart,  // a Run/Compaction turn-loop begins
  TransientAgentEnd,    // a Run/Compaction turn-loop ends
  TransientTurnStart,   // one provider+tool cycle begins
  TransientTurnEnd,     // one provider+tool cycle ends
  TransientStream,      // raw provider StreamEvent (message deltas)
  TransientToolStart,   // a prepared tool call is about to run
  TransientToolUpdate,
  TransientToolEnd,
  TransientRetryStart,  // a retry backoff is about to begin
  TransientRetryEnd,    // the attempt following a backoff has concluded
  TransientQueueUpdate, // steer/follow-up/next-turn queue depth changed
  TransientCancelRequested,  // AgentHarness_RequestCancel set the abort flag
};

struct AgentHarnessEvent {
  AgentHarnessEventKind Kind;
  u64 Cursor;
  SessionRecord Record;         // DurableRecord
  StreamEvent Stream;           // TransientStream
  StrView ToolCallId;           // tool_* events
  StrView ToolName;             // tool_* events
  StrView ToolArgsJson;         // TransientToolStart
  ToolResult ToolResultValue;   // TransientToolUpdate/TransientToolEnd
  bool IsError;                 // TransientToolEnd

  AgentTurnKind Turn;           // TransientAgentStart/End, TransientTurnStart/End
  AgentTurnOutcome Outcome;     // TransientAgentEnd, TransientTurnEnd
  StrView Error;                // TransientAgentEnd/TransientTurnEnd on failure

  RetryLayer Layer;             // TransientRetryStart/End
  u32 RetryAttempt;
  u32 RetryMaxAttempts;
  u32 RetryDelayMs;
  StrView RetryError;

  u32 SteerCount;                // TransientQueueUpdate
  u32 FollowUpCount;
  u32 NextTurnCount;
};

struct AgentHarnessSnapshot {
  SessionReduction State;
  Array<Message> Transcript;
  u64 Cursor;
  bool Running;
  bool Faulted;
};

struct AgentHarnessWatchSlot {
  Arena Mem;
  AgentHarnessEvent Event;
};

struct AgentHarnessWatch {
  AgentHarness* Owner;
  DurableId Id;
  Mutex Lock;
  CondVar Changed;
  AgentHarnessWatchSlot Slots[AGENT_HARNESS_MAX_WATCH_EVENTS];
  u32 Capacity;
  u32 Head;
  u32 Count;
  u64 Cursor;
  bool Initialized;
  bool Registered;
  bool Lagged;
  bool Closed;
  u32 Waiting;
};
