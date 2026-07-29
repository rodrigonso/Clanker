#include "agent/primitives.h"

#include "llm/retry.h"

namespace {

constexpr usize WORKER_ARENA_RESERVE = (usize)1 << 30;
constexpr u32 DEFAULT_MAX_PARALLEL = 8;

const ToolRuntime* FindTool(const ToolRuntime* tools, u32 toolCount,
                            StrView name) {
  for (u32 i = 0; i < toolCount; i++)
    if (StrView_Eq(tools[i].Definition->Name, name)) return &tools[i];
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

void SetPreparedError(Arena* arena, AgentPreparedTool* prepared,
                      StrView message) {
  prepared->Result = Array_Make<Content>(arena, 1);
  Array_Push(&prepared->Result, Content_Text(StrView_Dup(arena, message)));
  prepared->IsError = true;
  prepared->Runnable = false;
}

void PrepareOne(Arena* arena, const ToolRuntime* tools, u32 toolCount,
                const AgentToolHooks* hooks, AgentPreparedTool* prepared) {
  const Content* call = prepared->Call;
  const ToolRuntime* runtime = FindTool(tools, toolCount, call->ToolName);
  prepared->EffectiveArgumentsJson = SVL("{}");
  prepared->ReplayPolicy = ToolReplayPolicy::NoEffect;
  JsonValue* arguments = Json_Parse(arena, call->ToolArgsJson);
  StrBuf canonical = StrBuf_Make(arena, call->ToolArgsJson.Length + 32);
  if (Json_IsObject(arguments)) {
    Json_WriteValue(&canonical, arguments);
    prepared->EffectiveArgumentsJson = StrBuf_View(&canonical);
  }
  if (!runtime) {
    StrBuf error = StrBuf_Make(arena, 64);
    StrBuf_AppendCStr(&error, "Tool not found: ");
    StrBuf_Append(&error, call->ToolName);
    SetPreparedError(arena, prepared, StrBuf_View(&error));
    return;
  }
  prepared->Runtime = runtime;
  prepared->Resolved = runtime->Definition;

  if (!arguments) {
    SetPreparedError(arena, prepared, SVL("Tool arguments are invalid JSON"));
    return;
  }
  if (runtime->Definition->PrepareArguments) {
    JsonValue* normalized = arguments;
    StrBuf error = StrBuf_Make(arena, 96);
    if (!runtime->Definition->PrepareArguments(
            runtime->Definition->Ctx, arena, arguments, &normalized, &error) ||
        !normalized) {
      SetPreparedError(
          arena, prepared,
          error.Length ? StrBuf_View(&error)
                       : SVL("Tool argument preparation failed"));
      return;
    }
    arguments = normalized;
  }

  if (Json_IsObject(arguments)) {
    StrBuf_Clear(&canonical);
    Json_WriteValue(&canonical, arguments);
    prepared->EffectiveArgumentsJson = StrBuf_View(&canonical);
  }

  StrBuf validationError = StrBuf_Make(arena, 128);
  if (!JsonSchema_Validate(arena, &runtime->Schema, arguments,
                           &validationError)) {
    StrBuf error = StrBuf_Make(arena, validationError.Length + 96);
    StrBuf_AppendCStr(&error, "Validation failed for tool \"");
    StrBuf_Append(&error, call->ToolName);
    StrBuf_AppendCStr(&error, "\":\n  - ");
    StrBuf_Append(&error, StrBuf_View(&validationError));
    SetPreparedError(arena, prepared, StrBuf_View(&error));
    return;
  }

  StrBuf_Clear(&canonical);
  Json_WriteValue(&canonical, arguments);
  prepared->Args.Root = arguments;
  prepared->Args.Json = StrBuf_View(&canonical);
  prepared->EffectiveArgumentsJson = prepared->Args.Json;

  StrView reason = {};
  if (hooks && hooks->BeforeTool &&
      !hooks->BeforeTool(hooks->User, arena, call->ToolName,
                         &prepared->Args, &reason)) {
    StrBuf error = StrBuf_Make(arena, 64);
    StrBuf_AppendCStr(&error, reason.Length ? "" : "Tool blocked");
    StrBuf_Append(&error, reason);
    SetPreparedError(arena, prepared, StrBuf_View(&error));
    return;
  }
  prepared->ReplayPolicy = prepared->Resolved->ReplayPolicy;
  prepared->Runnable = true;
}

struct ToolUpdateBridge {
  AgentPreparedTool* Prepared;
  const AgentToolEventSink* Events;
};

void PublishToolUpdate(void* user, const ToolResult* partial) {
  ToolUpdateBridge* bridge = (ToolUpdateBridge*)user;
  if (bridge->Events && bridge->Events->OnUpdate)
    bridge->Events->OnUpdate(
        bridge->Events->User, bridge->Prepared->Call->ToolId,
        bridge->Prepared->Call->ToolName, partial);
}

void RunOne(AgentPreparedTool* prepared, Arena* arena,
            const AgentToolEventSink* events) {
  const Content* call = prepared->Call;
  StrBuf error = StrBuf_Make(arena, 64);
  ToolResult result = {};
  result.Content = Array_Make<Content>(arena, 1);
  bool acceptingUpdates = true;
  ToolUpdateBridge bridge = {prepared, events};
  ToolUpdateSink updates = {};
  updates.User = &bridge;
  updates.Accepting = &acceptingUpdates;
  updates.OnUpdate = PublishToolUpdate;
  bool ok = prepared->Resolved->Execute(
      prepared->Resolved->Ctx, arena, call->ToolId, &prepared->Args,
      prepared->Abort, &updates, &result, &error);
  acceptingUpdates = false;
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
  u32* Completed;
  u32 Count;
  u32 Next;
  u32 CompletedCount;
  Mutex Lock;
  CondVar Changed;
  const AgentToolEventSink* Events;
};

void ToolWorker(void* argument) {
  ToolPool* pool = (ToolPool*)argument;
  for (;;) {
    Mutex_Lock(&pool->Lock);
    u32 index = pool->Next;
    if (index < pool->Count) pool->Next = index + 1;
    Mutex_Unlock(&pool->Lock);
    if (index >= pool->Count) break;
    RunOne(pool->Jobs[index], &pool->Jobs[index]->Worker, pool->Events);
    Mutex_Lock(&pool->Lock);
    pool->Completed[pool->CompletedCount++] = index;
    Cond_Signal(&pool->Changed);
    Mutex_Unlock(&pool->Lock);
  }
}

void FinalizeOne(Arena* arena, AgentPreparedTool* prepared,
                 const AgentToolHooks* hooks,
                 const AgentToolEventSink* events,
                 AgentToolCallResult* out) {
  Array<Content> content =
      prepared->HasWorker ? RelocateContentArray(arena, prepared->Result)
                          : prepared->Result;
  ToolOutcome outcome = {};
  outcome.Content = content;
  outcome.IsError = prepared->IsError;
  outcome.Terminate = prepared->Terminate;
  if (hooks && hooks->AfterTool)
    hooks->AfterTool(hooks->User, arena, prepared->Call->ToolName,
                     &prepared->Args, &outcome);

  AgentToolCallResult call = {};
  call.ToolCallId = prepared->Call->ToolId;
  call.ToolName = prepared->Call->ToolName;
  call.EffectiveArgumentsJson = prepared->EffectiveArgumentsJson;
  call.ReplayPolicy = prepared->ReplayPolicy;
  call.Content = outcome.Content;
  call.IsError = outcome.IsError;
  call.Terminate = outcome.Terminate;
  *out = call;

  if (events && events->OnEnd) {
    ToolResult result = {};
    result.Content = call.Content;
    result.Terminate = call.Terminate;
    events->OnEnd(events->User, call.ToolCallId, call.ToolName, &result,
                  call.IsError);
  }
  if (prepared->HasWorker) {
    ArenaRelease(&prepared->Worker);
    prepared->HasWorker = false;
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

void AgentToolBatch_Prepare(Arena* arena, const ToolRuntime* tools, u32 toolCount,
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

void AgentToolBatch_Run(Arena* arena, AgentToolBatchPlan* plan,
                        u32 maxParallelTools, const AgentToolHooks* hooks,
                        const AgentToolEventSink* events,
                        AgentToolBatchResult* out) {
  if (!arena || !plan || !out) return;
  AgentToolBatchResult result = {};
  result.Calls =
      Array_Make<AgentToolCallResult>(arena, plan->Calls.Count
                                                ? plan->Calls.Count
                                                : 1);
  result.Calls.Count = plan->Calls.Count;
  for (u32 i = 0; i < plan->Calls.Count; i++)
    result.Calls.Data[i] = AgentToolCallResult{};
  result.AnyTool = plan->Calls.Count > 0;
  result.AllTerminate = result.AnyTool;

  for (u32 i = 0; i < plan->Calls.Count; i++) {
    if (!plan->Calls.Data[i].Runnable) {
      FinalizeOne(arena, &plan->Calls.Data[i], hooks, events,
                  &result.Calls.Data[i]);
      if (!result.Calls.Data[i].Terminate) result.AllTerminate = false;
    }
  }

  if (plan->RunnableCount == 0) {
    *out = result;
    return;
  }
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
    ToolPool pool = {};
    pool.Jobs = runnable;
    pool.Completed =
        ArenaPushArray(plan->Calls.Backing, u32, plan->RunnableCount);
    pool.Count = plan->RunnableCount;
    pool.Events = events;
    Mutex_Init(&pool.Lock);
    Cond_Init(&pool.Changed);
    Thread* threads = ArenaPushArray(plan->Calls.Backing, Thread, totalWorkers);
    u32 started = 0;
    for (u32 i = 0; i < totalWorkers; i++) {
      if (Thread_Start(&threads[started], ToolWorker, &pool)) started++;
    }
    if (started == 0) {
      for (u32 i = 0; i < plan->RunnableCount; i++) {
        RunOne(runnable[i], &runnable[i]->Worker, events);
        FinalizeOne(arena, runnable[i], hooks, events,
                    &result.Calls.Data[runnable[i] - plan->Calls.Data]);
        if (!result.Calls.Data[runnable[i] - plan->Calls.Data].Terminate)
          result.AllTerminate = false;
      }
    } else {
      u32 handled = 0;
      while (handled < plan->RunnableCount) {
        Mutex_Lock(&pool.Lock);
        while (handled >= pool.CompletedCount)
          Cond_Wait(&pool.Changed, &pool.Lock);
        u32 completed = pool.Completed[handled++];
        Mutex_Unlock(&pool.Lock);
        AgentPreparedTool* prepared = runnable[completed];
        u32 sourceIndex = (u32)(prepared - plan->Calls.Data);
        FinalizeOne(arena, prepared, hooks, events,
                    &result.Calls.Data[sourceIndex]);
        if (!result.Calls.Data[sourceIndex].Terminate)
          result.AllTerminate = false;
      }
    }
    for (u32 i = 0; i < started; i++) Thread_Join(&threads[i]);
    Cond_Destroy(&pool.Changed);
    Mutex_Destroy(&pool.Lock);
  } else {
    for (u32 i = 0; i < plan->Calls.Count; i++) {
      if (!plan->Calls.Data[i].Runnable) continue;
      RunOne(&plan->Calls.Data[i], plan->Calls.Backing, events);
      FinalizeOne(arena, &plan->Calls.Data[i], hooks, events,
                  &result.Calls.Data[i]);
      if (!result.Calls.Data[i].Terminate) result.AllTerminate = false;
    }
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
