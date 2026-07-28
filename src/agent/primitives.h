#pragma once

#include "agent/tool.h"
#include "base/thread.h"
#include "llm/types.h"

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
                     StrView argsJson, StrView* outReason);
  void (*AfterTool)(void* user, Arena* arena, StrView toolName,
                    StrView argsJson, ToolOutcome* io);
};

struct AgentPreparedTool {
  const Content* Call;
  const Tool* Resolved;
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
  ToolReplayPolicy ReplayPolicy;
  Array<Content> Content;
  bool IsError;
  bool Terminate;
};

struct AgentToolBatchResult {
  Array<AgentToolCallResult> Calls;
  bool AnyTool;
  bool AllTerminate;
};

// Preparation performs only resolution and hooks. AgentToolBatch_Run is the
// external-effect boundary. Finalize relocates worker results and applies
// after-tool hooks in original call order.
void AgentToolBatch_Prepare(Arena* arena, const Tool* tools, u32 toolCount,
                            const Message* assistant, ToolExecution execution,
                            volatile i32* abort, const AgentToolHooks* hooks,
                            AgentToolBatchPlan* out);
void AgentToolBatch_Run(AgentToolBatchPlan* plan, u32 maxParallelTools);
void AgentToolBatch_Finalize(Arena* arena, AgentToolBatchPlan* plan,
                             const AgentToolHooks* hooks,
                             AgentToolBatchResult* out);

Message AgentToolResult_Materialize(const AgentToolCallResult* result);

bool AgentCheckpoint_ContinueTurn(bool anyTool, bool allToolsTerminate,
                                  u32 steeringCount);
bool AgentCheckpoint_ContinueRun(u32 followUpCount);
