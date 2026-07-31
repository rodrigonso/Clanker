#pragma once

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
  StreamSink StreamEvents;
  ToolExecution ToolExec;
  u32 MaxParallelTools;
  AgentRetryPolicy Retry;
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
  Arena Work;     // per-run scratch; reset after each Run/Compact/Navigate call
  ToolRuntime* RuntimeTools;
  Mutex StateLock;  // guards Records/Reduction/Queues/Running/Faulted/Watchers

  // Loaded once at Init (SessionLog_ReadAll -> SessionReducer_Reduce) and
  // folded in O(1) per appended record thereafter -- never reloaded from the
  // repository on a per-turn/per-append path. See AgentHarness_Append.
  Array<SessionRecord> Records;
  SessionReduction Reduction;
  AgentHarnessQueues Queues;

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
SessionResult AgentHarness_Run(AgentHarness* harness, StrView prompt);
SessionResult AgentHarness_Compact(AgentHarness* harness);
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
