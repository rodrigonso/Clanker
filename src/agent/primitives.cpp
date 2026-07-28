#include "agent/primitives.h"

#include "llm/retry.h"

namespace {

constexpr usize WORKER_ARENA_RESERVE = (usize)1 << 30;
constexpr u32 DEFAULT_MAX_PARALLEL = 8;

const Tool* FindTool(const Tool* tools, u32 toolCount, StrView name) {
  for (u32 i = 0; i < toolCount; i++)
    if (StrView_Eq(tools[i].Name, name)) return &tools[i];
  return nullptr;
}

bool ShouldReplayMessage(const Message& message) {
  return message.Kind != Role::Assistant ||
         (message.Stop != StopReason::Error &&
          message.Stop != StopReason::Aborted);
}

Content RelocateContent(Arena* arena, const Content& source) {
  Content result = source;
  result.Text = StrView_Dup(arena, source.Text);
  result.Signature = StrView_Dup(arena, source.Signature);
  result.ToolId = StrView_Dup(arena, source.ToolId);
  result.ToolName = StrView_Dup(arena, source.ToolName);
  result.ToolArgsJson = StrView_Dup(arena, source.ToolArgsJson);
  result.ImageData = StrView_Dup(arena, source.ImageData);
  result.ImageMimeType = StrView_Dup(arena, source.ImageMimeType);
  return result;
}

Array<Content> RelocateContentArray(Arena* arena, const Array<Content>& source) {
  Array<Content> result =
      Array_Make<Content>(arena, source.Count ? source.Count : 1);
  for (u32 i = 0; i < source.Count; i++)
    Array_Push(&result, RelocateContent(arena, source.Data[i]));
  return result;
}

void PrepareOne(Arena* arena, const Tool* tools, u32 toolCount,
                const AgentToolHooks* hooks, AgentPreparedTool* prepared) {
  const Content* call = prepared->Call;
  const Tool* tool = FindTool(tools, toolCount, call->ToolName);
  if (!tool) {
    StrBuf error = StrBuf_Make(arena, 64);
    StrBuf_AppendCStr(&error, "Tool not found: ");
    StrBuf_Append(&error, call->ToolName);
    prepared->Result = Array_Make<Content>(arena, 1);
    Array_Push(&prepared->Result, Content_Text(StrBuf_View(&error)));
    prepared->IsError = true;
    return;
  }
  StrView reason = {};
  if (hooks && hooks->BeforeTool &&
      !hooks->BeforeTool(hooks->User, arena, call->ToolName,
                         call->ToolArgsJson, &reason)) {
    StrBuf error = StrBuf_Make(arena, 64);
    StrBuf_AppendCStr(&error, reason.Length ? "" : "Tool blocked");
    StrBuf_Append(&error, reason);
    prepared->Result = Array_Make<Content>(arena, 1);
    Array_Push(&prepared->Result, Content_Text(StrBuf_View(&error)));
    prepared->IsError = true;
    return;
  }
  prepared->Resolved = tool;
  prepared->Runnable = true;
}

void RunOne(AgentPreparedTool* prepared, Arena* arena) {
  const Content* call = prepared->Call;
  StrBuf error = StrBuf_Make(arena, 64);
  ToolResult result = {};
  result.Content = Array_Make<Content>(arena, 1);
  bool ok = prepared->Resolved->Execute(
      prepared->Resolved->Ctx, arena, call->ToolId, call->ToolArgsJson,
      prepared->Abort, &result, &error);
  if (ok) {
    prepared->Result = result.Content;
    prepared->Terminate = result.Terminate;
    prepared->IsError = false;
  } else {
    prepared->Result = Array_Make<Content>(arena, 1);
    Array_Push(&prepared->Result,
               Content_Text(StrView_Dup(arena, StrBuf_View(&error))));
    prepared->IsError = true;
    prepared->Terminate = false;
  }
}

struct ToolPool {
  AgentPreparedTool** Jobs;
  u32 Count;
  u32 Next;
  Mutex Lock;
};

void ToolWorker(void* argument) {
  ToolPool* pool = (ToolPool*)argument;
  for (;;) {
    Mutex_Lock(&pool->Lock);
    u32 index = pool->Next;
    if (index < pool->Count) pool->Next = index + 1;
    Mutex_Unlock(&pool->Lock);
    if (index >= pool->Count) break;
    RunOne(pool->Jobs[index], &pool->Jobs[index]->Worker);
  }
}

}  // namespace

void AgentProvider_BuildRequest(Arena* arena, StrView systemPrompt,
                                const Message* messages, u32 messageCount,
                                const Tool* tools, u32 toolCount,
                                AgentProviderRequest* out) {
  AgentProviderRequest request = {};
  Array<Message> replay =
      Array_Make<Message>(arena, messageCount ? messageCount : 1);
  for (u32 i = 0; i < messageCount; i++) {
    if (ShouldReplayMessage(messages[i])) Array_Push(&replay, messages[i]);
  }
  if (toolCount) {
    request.ToolDefs = ArenaPushArray(arena, ToolDef, toolCount);
    for (u32 i = 0; i < toolCount; i++)
      request.ToolDefs[i] = Tool_ToDef(tools[i]);
  }
  request.Context.SystemPrompt = systemPrompt;
  request.Context.Messages = replay.Data;
  request.Context.MessageCount = replay.Count;
  request.Context.Tools = request.ToolDefs;
  request.Context.ToolCount = toolCount;
  *out = request;
}

AgentProviderAttemptResult AgentProvider_RunAttempt(
    const AgentProviderAttemptInput* input) {
  AgentProviderAttemptResult result = {};
  StreamOptions options = input->Options;
  if (input->ResolveAuth) {
    StrView token = {};
    if (input->ResolveAuth(input->AuthUser, input->Arena,
                           input->ModelInfo->Provider, options.Abort, &token))
      options.ApiKey = token;
  }
  if (options.Abort && *options.Abort) {
    result.Assistant.Kind = Role::Assistant;
    result.Assistant.Stop = StopReason::Aborted;
    return result;
  }
  result.Ok = input->ProviderImpl.Stream(
      input->ProviderImpl.Ctx, input->Arena, input->ModelInfo, input->Context,
      &options, input->Sink, &result.Assistant);
  return result;
}

AgentRetryDecision AgentProvider_RetryDecision(const AgentRetryPolicy* policy,
                                               u32 attemptIndex,
                                               bool providerOk,
                                               const Message* assistant) {
  AgentRetryDecision decision = {};
  bool error = !providerOk || assistant->Stop == StopReason::Error;
  bool aborted = assistant->Stop == StopReason::Aborted;
  decision.Retry =
      policy->Enabled && error && !aborted && attemptIndex < policy->MaxRetries &&
      assistant->Content.Count == 0 && IsRetryableError(assistant->ErrorMessage);
  if (!decision.Retry) return decision;
  u32 shift = attemptIndex < 20 ? attemptIndex : 20;
  u64 delay = (u64)policy->BaseDelayMs << shift;
  if (policy->MaxDelayMs && delay > policy->MaxDelayMs)
    delay = policy->MaxDelayMs;
  decision.DelayMs = (u32)delay;
  return decision;
}

bool AgentProvider_SleepBackoff(u32 delayMs, volatile i32* abort) {
  constexpr u32 slice = 50;
  u32 elapsed = 0;
  while (elapsed < delayMs) {
    if (abort && *abort) return true;
    u32 step = delayMs - elapsed < slice ? delayMs - elapsed : slice;
    Sleep_Ms(step);
    elapsed += step;
  }
  return abort && *abort;
}

void AgentToolBatch_Prepare(Arena* arena, const Tool* tools, u32 toolCount,
                            const Message* assistant, ToolExecution execution,
                            volatile i32* abort, const AgentToolHooks* hooks,
                            AgentToolBatchPlan* out) {
  u32 count = 0;
  for (u32 i = 0; i < assistant->Content.Count; i++)
    if (assistant->Content.Data[i].Kind == ContentKind::ToolCall) count++;

  AgentToolBatchPlan plan = {};
  plan.Calls = Array_Make<AgentPreparedTool>(arena, count ? count : 1);
  plan.Sequential = execution == ToolExecution::Sequential;
  for (u32 i = 0; i < assistant->Content.Count; i++) {
    const Content* content = &assistant->Content.Data[i];
    if (content->Kind != ContentKind::ToolCall) continue;
    AgentPreparedTool prepared = {};
    prepared.Call = content;
    prepared.Abort = abort;
    PrepareOne(arena, tools, toolCount, hooks, &prepared);
    if (prepared.Runnable) {
      plan.RunnableCount++;
      if (prepared.Resolved->ExecMode == ToolExecMode::Sequential)
        plan.Sequential = true;
    }
    Array_Push(&plan.Calls, prepared);
  }
  *out = plan;
}

void AgentToolBatch_Run(AgentToolBatchPlan* plan, u32 maxParallelTools) {
  if (!plan || plan->RunnableCount == 0) return;
  if (!plan->Sequential && plan->RunnableCount >= 2) {
    AgentPreparedTool** runnable =
        ArenaPushArray(plan->Calls.Backing, AgentPreparedTool*, plan->RunnableCount);
    u32 index = 0;
    for (u32 i = 0; i < plan->Calls.Count; i++) {
      if (!plan->Calls.Data[i].Runnable) continue;
      plan->Calls.Data[i].Worker = ArenaCreate(WORKER_ARENA_RESERVE);
      plan->Calls.Data[i].HasWorker = true;
      runnable[index++] = &plan->Calls.Data[i];
    }
    u32 cap = maxParallelTools ? maxParallelTools : DEFAULT_MAX_PARALLEL;
    u32 totalWorkers = plan->RunnableCount < cap ? plan->RunnableCount : cap;
    u32 threadCount = totalWorkers > 1 ? totalWorkers - 1 : 0;
    ToolPool pool = {};
    pool.Jobs = runnable;
    pool.Count = plan->RunnableCount;
    Mutex_Init(&pool.Lock);
    Thread* threads =
        threadCount ? ArenaPushArray(plan->Calls.Backing, Thread, threadCount)
                    : nullptr;
    u32 started = 0;
    for (u32 i = 0; i < threadCount; i++) {
      if (Thread_Start(&threads[started], ToolWorker, &pool)) started++;
    }
    ToolWorker(&pool);
    for (u32 i = 0; i < started; i++) Thread_Join(&threads[i]);
    Mutex_Destroy(&pool.Lock);
  } else {
    for (u32 i = 0; i < plan->Calls.Count; i++)
      if (plan->Calls.Data[i].Runnable)
        RunOne(&plan->Calls.Data[i], plan->Calls.Backing);
  }
}

void AgentToolBatch_Finalize(Arena* arena, AgentToolBatchPlan* plan,
                             const AgentToolHooks* hooks,
                             AgentToolBatchResult* out) {
  AgentToolBatchResult result = {};
  result.Calls =
      Array_Make<AgentToolCallResult>(arena, plan->Calls.Count ? plan->Calls.Count : 1);
  result.AnyTool = plan->Calls.Count > 0;
  result.AllTerminate = result.AnyTool;

  for (u32 i = 0; i < plan->Calls.Count; i++) {
    AgentPreparedTool* prepared = &plan->Calls.Data[i];
    Array<Content> content =
        prepared->HasWorker ? RelocateContentArray(arena, prepared->Result)
                            : prepared->Result;
    ToolOutcome outcome = {};
    outcome.Content = content;
    outcome.IsError = prepared->IsError;
    outcome.Terminate = prepared->Terminate;
    if (hooks && hooks->AfterTool)
      hooks->AfterTool(hooks->User, arena, prepared->Call->ToolName,
                       prepared->Call->ToolArgsJson, &outcome);

    AgentToolCallResult call = {};
    call.ToolCallId = prepared->Call->ToolId;
    call.ToolName = prepared->Call->ToolName;
    call.EffectiveArgumentsJson = prepared->Call->ToolArgsJson;
    call.ReplayPolicy = prepared->Resolved
                            ? prepared->Resolved->ReplayPolicy
                            : ToolReplayPolicy::ManualReconcile;
    call.Content = outcome.Content;
    call.IsError = outcome.IsError;
    call.Terminate = outcome.Terminate;
    if (!call.Terminate) result.AllTerminate = false;
    Array_Push(&result.Calls, call);
    if (prepared->HasWorker) ArenaRelease(&prepared->Worker);
  }
  *out = result;
}

Message AgentToolResult_Materialize(const AgentToolCallResult* result) {
  Message message = {};
  message.Kind = Role::ToolResult;
  message.Content = result->Content;
  message.ToolCallId = result->ToolCallId;
  message.ToolName = result->ToolName;
  message.IsError = result->IsError;
  message.Timestamp = NowMillis();
  return message;
}

bool AgentCheckpoint_ContinueTurn(bool anyTool, bool allToolsTerminate,
                                  u32 steeringCount) {
  return steeringCount > 0 || (anyTool && !allToolsTerminate);
}

bool AgentCheckpoint_ContinueRun(u32 followUpCount) {
  return followUpCount > 0;
}
