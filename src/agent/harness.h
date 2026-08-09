#pragma once

#include "agent/compaction.h"
#include "agent/harness_event.h"
#include "agent/hooks.h"
#include "agent/primitives.h"
#include "session/reducer.h"
#include "session/repository.h"

struct AgentHarnessConfig {
  StrView SystemPrompt;
  Model ModelInfo;
  const Tool* Tools;
  u32 ToolCount;
  Provider ProviderImpl;
  StreamOptions Stream;
  ToolExecution ToolExec;
  u32 MaxParallelTools;
  AgentRetryPolicy Retry;
  // Automatic compaction. Zero-initialized means disabled, so a harness only
  // compacts on its own when a caller opts in; the CLI enables it by default.
  AgentCompactionPolicy Compaction;
  AgentToolHooks ToolHooks;
  AgentHarnessHooks Hooks;
  void* AuthUser;
  AgentResolveAuthFn ResolveAuth;
};

// In-memory-only steer/follow-up/next-turn message queues (Pi semantics):
// held only in harness memory and folded into the tree at their checkpoint.
// Lost if the process exits before that checkpoint is reached; nothing here
// is ever itself persisted.
struct AgentHarnessQueues {
  Array<Message> Steering;
  Array<Message> FollowUp;
  Array<Message> NextTurn;
};

struct AgentHarness {
  SessionLog Log;
  AgentHarnessConfig Config;
  Arena Runtime;  // tool schemas; lives for the harness lifetime
  Arena State;    // maintained Records + Reduction/index; append-only, never reloaded
  Arena Queued;   // queued message text; recycled whenever all queues drain
  // Per-turn scratch: the materialized context, the built request, the
  // provider's response, and every per-turn hook allocation. Reset at the top
  // of each turn of a run, which is what keeps a long run's cost proportional
  // to one turn rather than to the number of turns times the conversation.
  // Nothing allocated here may outlive the turn that allocated it.
  Arena Work;
  // Per-run scratch for the handful of things that genuinely span turns: the
  // run's accumulated message list and a PrepareNextTurn context override.
  // Reset with Work at the end of each Run/Compact/Navigate.
  Arena RunMem;
  // Holds only the returned SessionResult's message, which callers read after
  // Work has been handed back. Separate from State because State is append-only
  // and a run that fails without persisting anything would otherwise leave a
  // permanent copy behind on every failure.
  Arena LastResult;
  ToolRuntime* RuntimeTools;
  Mutex StateLock;  // guards Records/Reduction/Queues/Running/Faulted/Watchers
  // Guards the watcher registry only (Watchers/WatcherCount and each entry's
  // registration state). Split from StateLock because publishing happens once
  // per streamed token while StateLock is also held for O(history) snapshot
  // work; sharing one lock made token delivery wait on it.
  //
  // Lock order is StateLock -> WatcherLock -> AgentHarnessWatch::Lock. Nothing
  // may acquire them in the other direction.
  Mutex WatcherLock;

  // Loaded once at Init (SessionLog_ReadAll -> SessionReducer_Reduce) and
  // folded in O(1) per appended record thereafter -- never reloaded from the
  // repository on a per-turn/per-append path. See AgentHarness_Append.
  Array<SessionRecord> Records;
  SessionReduction Reduction;
  AgentHarnessQueues Queues;

  // Set with Atomic_StoreI32 and read with Atomic_LoadI32; see base/thread.h.
  // The pointer is handed to providers and tools, which poll it the same way.
  volatile i32 AbortFlag;
  bool Running;
  bool Faulted;

  AgentHarnessWatch* Watchers[AGENT_HARNESS_MAX_WATCHERS];
  u32 WatcherCount;
  bool Initialized;
};

// Takes ownership of log. Loads and reduces the log exactly once; there is no
// suspended-operation state to resume or abort -- a crash can only ever leave
// a persisted assistant tool call without its result, which context
// preparation papers over transiently (see session/replay.h). Storage
// failure still faults the harness and aborts the current call.
SessionResult AgentHarness_Init(AgentHarness* harness, SessionLog log,
                                const AgentHarnessConfig* config);
SessionResult AgentHarness_Close(AgentHarness* harness);

SessionResult AgentHarness_Snapshot(Arena* arena, AgentHarness* harness,
                                    SessionReduction* out);
SessionResult AgentHarness_SnapshotFull(Arena* arena, AgentHarness* harness,
                                        AgentHarnessSnapshot* out);
SessionResult AgentHarness_Watch(Arena* arena, AgentHarness* harness,
                                 u32 capacity,
                                 AgentHarnessSnapshot* outSnapshot,
                                 AgentHarnessWatch* outWatch);
SessionResult AgentHarness_WatchNext(Arena* arena, AgentHarnessWatch* watch,
                                     AgentHarnessEvent* out);
SessionResult AgentHarness_WatchRequestClose(AgentHarnessWatch* watch);
SessionResult AgentHarness_WatchClose(AgentHarnessWatch* watch);
// A returned SessionResult's Message is owned by the harness and stays valid
// only until the next accepted Run/Compact/Navigate. Copy it if you need it to
// outlive that; the run's scratch memory is handed back to the OS on return.
SessionResult AgentHarness_Run(AgentHarness* harness, StrView prompt);
SessionResult AgentHarness_Compact(AgentHarness* harness,
                                   StrView instructions = StrView{});
SessionResult AgentHarness_Navigate(AgentHarness* harness, DurableId targetEntryId);
SessionResult AgentHarness_LabelBranch(AgentHarness* harness, StrView label);
SessionResult AgentHarness_SetName(AgentHarness* harness, StrView name);
SessionResult AgentHarness_SetModel(AgentHarness* harness, Model model,
                                    Provider provider,
                                    ModelRequestProfile profile);
SessionResult AgentHarness_Fork(Arena* arena, AgentHarness* harness,
                                SessionRepository* repository,
                                DurableId sourceHeadId,
                                DurableId requestedSessionId, StrView label,
                                SessionLog* outLog, SessionInfo* outInfo);
SessionResult AgentHarness_RequestCancel(AgentHarness* harness);

// Memory-only queues: each call appends to the in-process queue and returns
// once it is recorded there (no session-log append, no durable record).
// Steering is folded onto the tree after the next tool-result batch;
// follow-up after a run's turn-loop would otherwise stop; next-turn before
// the next accepted prompt. See AgentHarnessQueues.
SessionResult AgentHarness_QueueSteering(AgentHarness* harness, StrView text);
SessionResult AgentHarness_QueueFollowUp(AgentHarness* harness, StrView text);
SessionResult AgentHarness_QueueNextTurn(AgentHarness* harness, StrView text);
