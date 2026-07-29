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

struct AgentHarness {
  SessionLog Log;
  AgentHarnessConfig Config;
  Arena Runtime;
  Arena Work;
  ToolRuntime* RuntimeTools;
  Mutex StateLock;
  DurableId ActiveOperationId;
  volatile i32 AbortFlag;
  AgentHarnessWatch* Watchers[AGENT_HARNESS_MAX_WATCHERS];
  u32 WatcherCount;
  bool Initialized;
  bool Running;
  bool Suspended;
  bool Faulted;
};

// Takes ownership of log. Restore is side-effect free: an unfinished operation
// returns SessionStatus::Suspended and requires ResumeOperation or AbortOperation.
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
SessionResult AgentHarness_QueueSteering(AgentHarness* harness, StrView text);
SessionResult AgentHarness_QueueFollowUp(AgentHarness* harness, StrView text);
SessionResult AgentHarness_QueueNextTurn(AgentHarness* harness, StrView text);

SessionResult AgentHarness_ResumeOperation(AgentHarness* harness);
SessionResult AgentHarness_AbortOperation(AgentHarness* harness, StrView reason);
SessionResult AgentHarness_ReconcileTool(AgentHarness* harness,
                                         DurableId toolExecutionId,
                                         RecoveryResolution resolution,
                                         const Message* suppliedResult);
