#pragma once

#include "agent/tool.h"
#include "base/thread.h"
#include "llm/types.h"
#include "session/replay.h"

enum class ToolExecution : u8 { Parallel, Sequential };

struct ToolOutcome {
  Array<Content> Content;
  bool IsError;
  bool Terminate;
};

struct AgentProviderRequest {
  LlmContext Context;
  ToolDef* ToolDefs;
};

void AgentProvider_BuildRequest(Arena* arena, StrView systemPrompt,
                                const Message* messages, u32 messageCount,
                                const Tool* tools, u32 toolCount,
                                AgentProviderRequest* out);

typedef bool (*AgentResolveAuthFn)(void* user, Arena* arena, StrView provider,
                                   volatile i32* abort, StrView* outToken);

struct AgentProviderAttemptInput {
  Arena* Arena;
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

struct AgentRetryDecision {
  bool Retry;
  u32 DelayMs;
};

AgentRetryDecision AgentProvider_RetryDecision(const AgentRetryPolicy* policy,
                                               u32 attemptIndex,
                                               bool providerOk,
                                               const Message* assistant);
bool AgentProvider_SleepBackoff(u32 delayMs, volatile i32* abort);

struct AgentToolHooks {
  void* User;
  bool (*BeforeTool)(void* user, Arena* arena, StrView toolName,
                     const ToolArgs* args, StrView* outReason);
  void (*AfterTool)(void* user, Arena* arena, StrView toolName,
                    const ToolArgs* args, ToolOutcome* io);
};

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
  Array<Content> Content;
  bool IsError;
  bool Terminate;
};

struct AgentToolBatchResult {
  Array<AgentToolCallResult> Calls;
  bool AnyTool;
  bool AllTerminate;
};

// Preparation resolves tools, normalizes and validates arguments, and runs the
// before-tool gate. Run is the external-effect boundary; it finalizes each call
// on the coordinator thread as soon as that call completes.
void AgentToolBatch_Prepare(Arena* arena, const ToolRuntime* tools, u32 toolCount,
                            const Message* assistant, ToolExecution execution,
                            volatile i32* abort, const AgentToolHooks* hooks,
                            AgentToolBatchPlan* out);
void AgentToolBatch_Run(Arena* arena, AgentToolBatchPlan* plan,
                        u32 maxParallelTools, const AgentToolHooks* hooks,
                        const AgentToolDispatchGate* dispatch,
                        const AgentToolEventSink* events,
                        AgentToolBatchResult* out);

Message AgentToolResult_Materialize(const AgentToolCallResult* result);

bool AgentCheckpoint_ContinueTurn(bool anyTool, bool allToolsTerminate,
                                  u32 steeringCount);
bool AgentCheckpoint_ContinueRun(u32 followUpCount);
