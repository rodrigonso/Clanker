#pragma once

#include "agent/hooks.h"
#include "agent/tool.h"
#include "base/thread.h"
#include "session/reducer.h"

struct AgentHarness;

constexpr u32 AGENT_HARNESS_MAX_WATCHERS = 8;
constexpr u32 AGENT_HARNESS_MAX_WATCH_EVENTS = 256;

// Distinguishes Pi's two retry layers so a retry_start/retry_end event can
// say which one paused: ProviderRequest is the transport-level HTTP retry
// (Config.Stream.Retry / HttpRetryPolicy, handled inside the provider's
// Stream implementation); AssistantTurn is the harness's own whole-turn
// retry (Config.Retry / AgentRetryPolicy), driven by the agent loop.
enum class RetryLayer : u8 { ProviderRequest, AssistantTurn };

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
  TransientEventsDropped,    // watcher overflowed; N transient events were skipped
};

// Payload for the agent/turn lifecycle pairs. `Outcome` and `Error` are only
// meaningful on the *End kinds.
struct AgentHarnessTurnEvent {
  AgentTurnKind Kind;
  AgentTurnOutcome Outcome;
  StrView Error;
};

// Payload shared by tool_start/update/end. `ArgumentsJson` is set only on
// start; `Result` only on update/end; `IsError` only on end.
struct AgentHarnessToolEvent {
  StrView CallId;
  StrView Name;
  StrView ArgumentsJson;
  ToolResult Result;
  bool IsError;
};

struct AgentHarnessRetryEvent {
  RetryLayer Layer;
  u32 Attempt;
  u32 MaxAttempts;
  u32 DelayMs;
  StrView Error;
};

struct AgentHarnessQueueEvent {
  u32 SteerCount;
  u32 FollowUpCount;
  u32 NextTurnCount;
};

// Reports a gap in the transient stream. Durable records are never dropped, so
// a consumer's view of the session tree stays complete across one of these; only
// progress rendering missed something.
struct AgentHarnessDroppedEvent {
  u32 Count;
};

// One event is exactly one kind, so the payloads share storage. Every member is
// trivially copyable, which keeps `= {}` initialization and plain assignment
// valid without any active-member bookkeeping beyond `Kind`.
//
// `Record` is a pointer rather than an inline value: SessionRecord is 408 bytes
// on its own, and storing it inline would make every queued stream delta pay
// for it. The pointee is owned by whichever arena cloned the event (a watcher
// ring slot, or the caller's arena after WatchNext).
struct AgentHarnessEvent {
  AgentHarnessEventKind Kind;
  u64 Cursor;
  union {
    const SessionRecord* Record;   // DurableRecord
    StreamEvent Stream;            // TransientStream
    AgentHarnessTurnEvent Turn;    // Transient{Agent,Turn}{Start,End}
    AgentHarnessToolEvent Tool;    // TransientTool{Start,Update,End}
    AgentHarnessRetryEvent Retry;  // TransientRetry{Start,End}
    AgentHarnessQueueEvent Queue;  // TransientQueueUpdate
    AgentHarnessDroppedEvent Dropped;  // TransientEventsDropped
  };
};

struct AgentHarnessSnapshot {
  SessionReduction State;
  Array<Message> Transcript;
  u64 Cursor;
  bool Running;
  bool Faulted;
};

// One arena per watcher rather than one per ring slot. Every arena commits at
// 64 KiB granularity, so 256 per-slot arenas cost 16 MiB of committed pages to
// hold ~27 KB of events. The shared arena is reset whenever the ring drains,
// which is the common case between bursts.
struct AgentHarnessWatchSlot {
  AgentHarnessEvent Event;
};

struct AgentHarnessWatch {
  AgentHarness* Owner;
  DurableId Id;
  Mutex Lock;
  CondVar Changed;
  Arena Mem;
  AgentHarnessWatchSlot Slots[AGENT_HARNESS_MAX_WATCH_EVENTS];
  u32 Capacity;
  u32 Head;
  u32 Count;
  u64 Cursor;
  // Transient events discarded since the consumer last saw a gap marker.
  // Reported once, then cleared; the watcher keeps running.
  u32 DroppedTransient;
  bool Initialized;
  bool Registered;
  bool Lagged;
  bool Closed;
  u32 Waiting;
};
