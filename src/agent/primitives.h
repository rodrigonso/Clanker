#pragma once

#include "agent/hooks.h"
#include "agent/tool.h"
#include "base/thread.h"
#include "llm/types.h"
#include "session/replay.h"

struct AgentPermissionGate;

enum class ToolExecution : u8 { Parallel, Sequential };

struct AgentProviderRequest {
  LlmContext Context;
  ToolDef* ToolDefs;
};

void AgentProvider_BuildRequest(Arena* arena, StrView systemPrompt,
                                const Message* messages, u32 messageCount,
                                const Tool* tools, u32 toolCount,
                                AgentProviderRequest* out);

typedef bool (*AgentResolveAuthFn)(void* user, Arena* arena, StrView provider,
                                   volatile i32* abort, ProviderAuth* out);

struct AgentProviderAttemptInput {
  Arena* Mem;
  Provider ProviderImpl;
  const Model* ModelInfo;
  const LlmContext* Context;
  StreamOptions Options;
  StreamSink Sink;
  void* AuthUser;
  AgentResolveAuthFn ResolveAuth;
};

struct AgentProviderAttemptResult {
  Message Assistant;
  bool Ok;
};

AgentProviderAttemptResult AgentProvider_RunAttempt(
    const AgentProviderAttemptInput* input);

struct AgentRetryPolicy {
  bool Enabled;
  u32 MaxRetries;
  u32 BaseDelayMs;
  u32 MaxDelayMs;
};

// pi enables these agent-level retries; transport retries remain disabled.
constexpr u32 AGENT_RETRY_DEFAULT_MAX_RETRIES = 3;
constexpr u32 AGENT_RETRY_DEFAULT_BASE_DELAY_MS = 2000;
constexpr u32 AGENT_RETRY_DEFAULT_MAX_DELAY_MS = 8000;

inline AgentRetryPolicy AgentRetryPolicy_Default() {
  AgentRetryPolicy policy = {};
  policy.Enabled = true;
  policy.MaxRetries = AGENT_RETRY_DEFAULT_MAX_RETRIES;
  policy.BaseDelayMs = AGENT_RETRY_DEFAULT_BASE_DELAY_MS;
  policy.MaxDelayMs = AGENT_RETRY_DEFAULT_MAX_DELAY_MS;
  return policy;
}

struct AgentRetryDecision {
  bool Retry;
  u32 DelayMs;
};

// Bounds how many *consecutive autonomous* turns one run may take. pi has no
// such cap; this is a deliberate divergence, and a safety valve rather than a
// task-shaping knob -- the default is far above what any real task reaches.
//
// User-supplied steering and follow-up reset the count, so the budget only ever
// limits turns the model drove on its own. A zero MaxTurns means unlimited,
// which is the escape hatch for a caller that wants pi's exact behaviour back.
struct AgentTurnLimitPolicy {
  bool Enabled;
  u32 MaxTurns;
};

constexpr u32 AGENT_TURN_LIMIT_DEFAULT_MAX_TURNS = 100;

inline AgentTurnLimitPolicy AgentTurnLimitPolicy_Default() {
  AgentTurnLimitPolicy policy = {};
  policy.Enabled = true;
  policy.MaxTurns = AGENT_TURN_LIMIT_DEFAULT_MAX_TURNS;
  return policy;
}

// A policy only bounds anything when it is enabled and names a positive cap.
bool AgentTurnLimitPolicy_IsValid(const AgentTurnLimitPolicy* policy);

// True once `turnsUsed` consecutive autonomous turns have already run, meaning
// the next one would exceed the budget. Kept separate from the
// AgentCheckpoint_* rules below so pi's continuation logic stays untouched.
bool AgentTurnLimit_Reached(const AgentTurnLimitPolicy* policy, u32 turnsUsed);

AgentRetryDecision AgentProvider_RetryDecision(const AgentRetryPolicy* policy,
                                               u32 attemptIndex,
                                               bool providerOk,
                                               const Message* assistant);
bool AgentProvider_SleepBackoff(u32 delayMs, volatile i32* abort);

struct AgentToolEventSink {
  void* User;
  void (*OnUpdate)(void* user, StrView toolCallId, StrView toolName,
                   const ToolResult* partial);
  void (*OnEnd)(void* user, StrView toolCallId, StrView toolName,
                const ToolResult* result, bool isError);
};

struct AgentToolDispatchGate {
  void* User;
  bool (*TryStart)(void* user);
};

struct AgentPreparedTool {
  const Content* Call;
  const ToolRuntime* Runtime;
  const Tool* Resolved;
  ToolArgs Args;
  StrView EffectiveArgumentsJson;
  volatile i32* Abort;
  bool Runnable;
  bool HasWorker;
  Arena Worker;
  Array<Content> Result;
  bool IsError;
  bool Terminate;
};

struct AgentToolBatchPlan {
  Array<AgentPreparedTool> Calls;
  bool Sequential;
  u32 RunnableCount;
};

struct AgentToolCallResult {
  StrView ToolCallId;
  StrView ToolName;
  StrView EffectiveArgumentsJson;
  ContentArray Content;
  bool IsError;
  bool Terminate;
};

struct AgentToolBatchResult {
  Array<AgentToolCallResult> Calls;
  bool AnyTool;
  bool AllTerminate;
};

// Preparation resolves tools, normalizes and validates arguments, and runs the
// hooks' before-tool gate. Run is the external-effect boundary; it finalizes
// each call, including the after-tool hook, on the coordinator thread as soon
// as that call completes.
void AgentToolBatch_Prepare(Arena* arena, const ToolRuntime* tools, u32 toolCount,
                            const Message* assistant, ToolExecution execution,
                            volatile i32* abort, const AgentHarnessHooks* hooks,
                            const AgentPermissionGate* permissions,
                            AgentToolBatchPlan* out);
void AgentToolBatch_Run(Arena* arena, AgentToolBatchPlan* plan,
                        u32 maxParallelTools, const AgentHarnessHooks* hooks,
                        const AgentToolDispatchGate* dispatch,
                        const AgentToolEventSink* events,
                        AgentToolBatchResult* out);

Message AgentToolResult_Materialize(const AgentToolCallResult* result);

bool AgentCheckpoint_ContinueTurn(bool anyTool, bool allToolsTerminate,
                                  u32 steeringCount);
bool AgentCheckpoint_ContinueRun(u32 followUpCount);
