#include "agent/driver.h"

#include "agent/compaction.h"
#include "base/thread.h"
#include "json/json.h"
#include "llm/retry.h"

namespace {

bool WorkOwns(const Arena* arena, const void* data, usize bytes) {
  usize offset = (usize)data - (usize)arena->Base;
  return arena->Base && offset <= arena->Used && bytes <= arena->Used - offset;
}

StrView MutableText(Arena* arena, StrView text) {
  return !text.Length || WorkOwns(arena, text.Data, text.Length)
             ? text : StrView_Dup(arena, text);
}

template <typename T>
void OwnMutableArray(Arena* arena, Array<T>* array) {
  if (array->Backing == arena &&
      WorkOwns(arena, array->Data, (usize)array->Capacity * sizeof(T))) return;
  Array<T> owned = Array_Make<T>(arena, array->Count);
  for (u32 i = 0; i < array->Count; i++) Array_Push(&owned, array->Data[i]);
  *array = owned;
}

void OwnMutableMessage(Arena* arena, Message* message) {
  OwnMutableArray(arena, &message->Content);
  for (u32 i = 0; i < message->Content.Count; i++) {
    Content* content = &message->Content.Data[i];
    content->Text = MutableText(arena, content->Text);
    content->ToolId = MutableText(arena, content->ToolId);
    content->ToolName = MutableText(arena, content->ToolName);
    content->ToolArgsJson = MutableText(arena, content->ToolArgsJson);
    content->ImageData = MutableText(arena, content->ImageData);
    content->ImageMimeType = MutableText(arena, content->ImageMimeType);
    content->Signature = MutableText(arena, content->Signature);
  }
  message->ErrorMessage = MutableText(arena, message->ErrorMessage);
  message->Model = MutableText(arena, message->Model);
  message->Provider = MutableText(arena, message->Provider);
  message->ToolCallId = MutableText(arena, message->ToolCallId);
  message->ToolName = MutableText(arena, message->ToolName);
}

// Hooks may replace owned fields with borrowed slices/arrays. Recheck at each
// mutable boundary, but keep already detached Work storage across retries.
void OwnMutableMessages(Arena* arena, Array<Message>* messages) {
  OwnMutableArray(arena, messages);
  for (u32 i = 0; i < messages->Count; i++)
    OwnMutableMessage(arena, &messages->Data[i]);
}

void OwnMutableProviderInput(Arena* arena, AgentHarnessProviderInput* input) {
  Model* model = &input->ModelInfo;
  model->Id = MutableText(arena, model->Id);
  model->Name = MutableText(arena, model->Name);
  model->Provider = MutableText(arena, model->Provider);
  model->BaseUrl = MutableText(arena, model->BaseUrl);
  LlmContext* context = &input->Context;
  context->SystemPrompt = MutableText(arena, context->SystemPrompt);
  Array<Message> messages = {(Message*)context->Messages, context->MessageCount,
                             context->MessageCount, arena};
  OwnMutableMessages(arena, &messages);
  context->Messages = messages.Data;
  Array<ToolDef> tools = {(ToolDef*)context->Tools, context->ToolCount,
                          context->ToolCount, arena};
  OwnMutableArray(arena, &tools);
  for (u32 i = 0; i < tools.Count; i++) {
    ToolDef* tool = &tools.Data[i];
    tool->Name = MutableText(arena, tool->Name);
    tool->Description = MutableText(arena, tool->Description);
    tool->ParamsJsonSchema = MutableText(arena, tool->ParamsJsonSchema);
  }
  context->Tools = tools.Data;
  input->Options.Auth.Token = MutableText(arena, input->Options.Auth.Token);
  input->Options.Request.ToolName = MutableText(arena, input->Options.Request.ToolName);
}

void OwnMutableTurnContext(Arena* arena, AgentTurnContext* context) {
  if (context->Assistant) {
    Message* assistant = ArenaPushStruct(arena, Message);
    *assistant = *context->Assistant;
    OwnMutableMessage(arena, assistant);
    context->Assistant = assistant;
  }
  OwnMutableMessages(arena, &context->ToolResults);
  OwnMutableMessages(arena, &context->Context);
  OwnMutableMessages(arena, &context->NewMessages);
}

SessionRecord NewMessageRecord(DurableId parentId, const Message& value) {
  SessionRecord record = {};
  record.EntryId = DurableId_Generate();
  record.Kind = SessionRecordKind::Message;
  record.MessageEntry.ParentId = parentId;
  record.MessageEntry.Value = value;
  return record;
}

bool ResolveProviderInput(AgentHarnessProviderInput* input) {
  if (!input || input->ModelInfo.Provider.Length == 0 || input->ModelInfo.Id.Length == 0 ||
      input->Options.MaxTokens < 0 ||
      !Model_RequestProfileIsValid(&input->ModelInfo, input->Options.Profile))
    return false;
  input->Options.Profile =
      Model_ResolveRequestProfile(&input->ModelInfo, input->Options.Profile);
  return true;
}

// Forwards every provider StreamEvent to watchers as a transient harness
// event, exactly once per event, in delivery order. The watch stream is the
// only delivery path; consumers subscribe through AgentHarness_Watch.
void OnProviderStreamEvent(void* user, const StreamEvent& event) {
  AgentHarness* harness = (AgentHarness*)user;
  AgentHarness_PublishStream(harness, &event);
}

void OnProviderRequestRetry(void* user, const HttpRetryEvent* retry) {
  AgentHarness* harness = (AgentHarness*)user;
  AgentHarnessEventKind kind = retry->Kind == HttpRetryEventKind::Start
                                   ? AgentHarnessEventKind::TransientRetryStart
                                   : AgentHarnessEventKind::TransientRetryEnd;
  StrView error = retry->Error.Length ? retry->Error : SVL("retryable HTTP response");
  AgentHarness_PublishRetry(harness, kind, RetryLayer::ProviderRequest,
                            retry->Attempt, retry->MaxRetries, retry->DelayMs,
                            error);
}

void OnToolUpdateBridge(void* user, StrView toolCallId, StrView toolName,
                        const ToolResult* partial) {
  AgentHarness_PublishToolUpdate((AgentHarness*)user, toolCallId, toolName, partial);
}

void OnToolEndBridge(void* user, StrView toolCallId, StrView toolName,
                     const ToolResult* result, bool isError) {
  AgentHarness_PublishToolEnd((AgentHarness*)user, toolCallId, toolName, result, isError);
}

AgentToolEventSink ToolEvents(AgentHarness* harness) {
  AgentToolEventSink sink = {};
  sink.User = harness;
  sink.OnUpdate = OnToolUpdateBridge;
  sink.OnEnd = OnToolEndBridge;
  return sink;
}

bool TryStartTool(void* user) {
  AgentHarness* harness = (AgentHarness*)user;
  ScopedLock lock(&harness->StateLock);
  return !Atomic_LoadI32(&harness->AbortFlag);
}

struct TurnRunResult {
  Message Assistant;
  bool ProviderOk;
};

// Per-run, memory-only state: the messages this run has appended so far, plus
// the replacement model/effort/context PrepareNextTurn asked for. None of this
// is persisted and none of it touches AgentHarnessConfig, so overrides expire
// when the run returns. Only NewMessages uses RunMem: replacement owners must
// be reclaimable without discarding that run-long list of State-owned messages.
struct AgentRunState {
  Array<Message> NewMessages;
  bool HasContext;
  Array<Message> Context;
  // Stable arena addresses keep nested Array/StrBuf backing pointers valid.
  // Stage into the other slot before releasing anything the update may borrow.
  Arena ContextMem[2];
  u32 ContextSlot;
  bool HasModel;
  Model ModelInfo;
  Arena ModelMem;
  bool HasProfile;
  ModelRequestProfile Profile;
  // Consecutive turns the model drove on its own. Reset whenever the user
  // supplies steering or follow-up, so the budget bounds runaway loops rather
  // than long interactive sessions. Lives here, not in Work, because
  // ContinueRun resets Work on every iteration.
  u32 AutonomousTurns;
  // Of those, how many are already covered by budgets the user granted at the
  // limit prompt. Subtracted out so each grant buys a fresh MaxTurns while
  // AutonomousTurns stays the true count the notice reports.
  u32 GrantedTurns;
};

void AgentRunState_RecordMessages(AgentRunState* run, const Message* messages, u32 count) {
  if (!run) return;
  for (u32 i = 0; i < count; i++) {
    Array_Push(&run->NewMessages, messages[i]);
    // A live context override accumulates exactly like the branch it replaced,
    // matching pi's currentContext.messages.push(...): without this, every
    // later turn would re-send the frozen snapshot and a non-terminating tool
    // batch would repeat the identical request forever.
    if (run->HasContext) Array_Push(&run->Context, messages[i]);
  }
}

void AgentRunState_Record(AgentRunState* run, const SessionRecord* records, u32 count) {
  if (!run) return;
  for (u32 i = 0; i < count; i++) {
    if (records[i].Kind != SessionRecordKind::Message) continue;
    AgentRunState_RecordMessages(run, &records[i].MessageEntry.Value, 1);
  }
}

void AgentRunState_DropContext(AgentRunState* run) {
  run->HasContext = false;
  run->Context = {};
  ArenaRelease(&run->ContextMem[run->ContextSlot]);
}

bool AgentRunState_OwnsText(const AgentRunState* run, StrView text) {
  const Arena* owners[] = {&run->ContextMem[run->ContextSlot], &run->ModelMem};
  for (const Arena* owner : owners) {
    usize offset = (usize)text.Data - (usize)owner->Base;
    if (owner->Base && offset < owner->Used) return true;
  }
  return false;
}

SessionResult AgentRunState_Apply(AgentRunState* run,
                                  const AgentNextTurnUpdate* update) {
  if (!run || !update) return SessionResult_Ok();
  if (update->HasModel &&
      (update->ModelInfo.Provider.Length == 0 || update->ModelInfo.Id.Length == 0))
    return SessionResult_Error(SessionStatus::HookError,
                               SVL("prepare-next-turn hook produced an invalid model"));

  Arena modelMem = {};
  Model model = {};
  Array<Message> context = {};
  u32 contextSlot = run->ContextSlot ^ 1;
  if (update->HasModel) {
    modelMem = ArenaCreate();
    model = update->ModelInfo;
    model.Id = StrView_Dup(&modelMem, model.Id);
    model.Name = StrView_Dup(&modelMem, model.Name);
    model.Provider = StrView_Dup(&modelMem, model.Provider);
    model.BaseUrl = StrView_Dup(&modelMem, model.BaseUrl);
  }
  if (update->HasContext) {
    Arena* mem = &run->ContextMem[contextSlot];
    *mem = ArenaCreate();
    context = Array_Make<Message>(mem, update->Context.Count + 8);
    for (u32 i = 0; i < update->Context.Count; i++)
      Array_Push(&context, Message_Clone(mem, update->Context.Data[i]));
  }

  // Both fields may borrow either old owner or Work. Finish all copies before
  // releasing either owner, including when context and model borrow each other.
  if (update->HasModel) {
    ArenaRelease(&run->ModelMem);
    // Model contains only views, not arena-backed builders.
    run->ModelMem = modelMem;
    run->HasModel = true;
    run->ModelInfo = model;
  }
  if (update->HasProfile) {
    run->HasProfile = true;
    run->Profile = update->Profile;
  }
  if (update->HasContext) {
    AgentRunState_DropContext(run);
    run->ContextSlot = contextSlot;
    run->HasContext = true;
    run->Context = context;
  }
  return SessionResult_Ok();
}

typedef SessionResult (*AgentMessageSourceFn)(void* user, Arena* arena, Array<Message>* out);

// Polls a hook message source (pi's getSteeringMessages / getFollowUpMessages)
// and appends whatever it returns onto the branch exactly like a drained
// queue, so both sources reach the model at the same checkpoint.
SessionResult DrainMessageSource(AgentHarness* harness, AgentMessageSourceFn source,
                                 const char* failure, DurableId* ioHead,
                                 AgentRunState* run, u32* outCount) {
  *outCount = 0;
  if (!source) return SessionResult_Ok();
  Array<Message> messages = Array_Make<Message>(&harness->Work, 4);
  SessionResult hooked = AgentHook_Failure(
      source(harness->Config.Hooks.User, &harness->Work, &messages), failure);
  if (!SessionResult_IsOk(hooked)) return hooked;
  if (messages.Count == 0) return SessionResult_Ok();

  Array<SessionRecord> records = Array_Make<SessionRecord>(&harness->Work, messages.Count);
  DurableId head = *ioHead;
  for (u32 i = 0; i < messages.Count; i++) {
    SessionRecord record = NewMessageRecord(head, messages.Data[i]);
    Array_Push(&records, record);
    head = record.EntryId;
  }
  SessionResult appended = AgentHarness_Append(harness, records.Data, records.Count);
  if (!SessionResult_IsOk(appended)) return appended;
  *ioHead = head;
  *outCount = messages.Count;
  AgentRunState_Record(run, records.Data, records.Count);
  return SessionResult_Ok();
}

// A checkpoint drains the in-memory queue first and the hook source second, so
// a caller wiring up both sees queue-before-hook order on the branch. The
// returned count drives the same continue decision either source alone would.
// Both paths feed AgentRunState, so a queued message reaches a live context
// override just like a hook-sourced one does.
SessionResult FlushQueueIntoRun(AgentHarness* harness, Array<Message>* queue, DurableId* ioHead,
                                AgentRunState* run, u32* outCount) {
  Array<Message> drained = Array_Make<Message>(&harness->Work, 4);
  SessionResult result = AgentHarness_FlushQueue(harness, queue, ioHead, outCount, &drained);
  if (!SessionResult_IsOk(result)) return result;
  AgentRunState_RecordMessages(run, drained.Data, drained.Count);
  return SessionResult_Ok();
}

SessionResult FlushSteering(AgentHarness* harness, DurableId* ioHead, AgentRunState* run,
                            u32* outCount) {
  u32 queued = 0;
  SessionResult result =
      FlushQueueIntoRun(harness, &harness->Queues.Steering, ioHead, run, &queued);
  if (!SessionResult_IsOk(result)) return result;
  u32 hooked = 0;
  result = DrainMessageSource(harness, harness->Config.Hooks.GetSteeringMessages,
                              "steering-messages hook failed", ioHead, run, &hooked);
  if (!SessionResult_IsOk(result)) return result;
  *outCount = queued + hooked;
  return SessionResult_Ok();
}

SessionResult FlushFollowUp(AgentHarness* harness, DurableId* ioHead, AgentRunState* run,
                            u32* outCount) {
  u32 queued = 0;
  SessionResult result =
      FlushQueueIntoRun(harness, &harness->Queues.FollowUp, ioHead, run, &queued);
  if (!SessionResult_IsOk(result)) return result;
  u32 hooked = 0;
  result = DrainMessageSource(harness, harness->Config.Hooks.GetFollowUpMessages,
                              "follow-up-messages hook failed", ioHead, run, &hooked);
  if (!SessionResult_IsOk(result)) return result;
  *outCount = queued + hooked;
  return SessionResult_Ok();
}

// Builds the active-branch context (linear in branch length, never full
// history), runs the BeforeContext/ConvertToLlm/BeforeProvider/AfterProvider
// hooks, and drives the assistant-turn retry layer (Config.Retry /
// AgentRetryPolicy), emitting transient retry_start/retry_end events around
// each backoff. `run`, when given, supplies this run's PrepareNextTurn
// overrides. Never appends anything -- the caller decides what a
// successful/failed attempt means for the tree (a plain assistant message for
// a run turn, or a Compaction entry for compaction).
SessionResult RunAssistantTurn(AgentHarness* harness, AgentTurnKind turnKind, DurableId headId,
                               const Message* extraTrailingMessage,
                               const AgentRunState* run, TurnRunResult* out) {
  Array<Message> contextMessages = {};
  if (run && run->HasContext) {
    contextMessages =
        Array_Make<Message>(&harness->Work, run->Context.Count ? run->Context.Count : 1);
    for (u32 i = 0; i < run->Context.Count; i++)
      Array_Push(&contextMessages, run->Context.Data[i]);
  } else {
    SessionReduction reduction = AgentHarness_ReduceSnapshot(harness);
    SessionResult contextResult =
        SessionReducer_Context(&harness->Work, &reduction, headId, &contextMessages);
    if (!SessionResult_IsOk(contextResult)) return contextResult;
  }
  if (extraTrailingMessage) Array_Push(&contextMessages, *extraTrailingMessage);

  if (harness->Config.Hooks.BeforeContext) {
    OwnMutableMessages(&harness->Work, &contextMessages);
    SessionResult hooked = AgentHook_Failure(
        harness->Config.Hooks.BeforeContext(harness->Config.Hooks.User, &harness->Work,
                                            turnKind, headId, &contextMessages),
        "before-context hook failed");
    if (!SessionResult_IsOk(hooked)) return hooked;
  }

  if (harness->Config.Hooks.ConvertToLlm) {
    OwnMutableMessages(&harness->Work, &contextMessages);
    SessionResult hooked = AgentHook_Failure(
        harness->Config.Hooks.ConvertToLlm(harness->Config.Hooks.User, &harness->Work,
                                           turnKind, headId, &contextMessages),
        "convert-to-llm hook failed");
    if (!SessionResult_IsOk(hooked)) return hooked;
  }

  AgentProviderRequest request = {};
  StrView effectiveSystemPrompt = AgentPermission_AppendSystemPrompt(
      &harness->Work, harness->Config.SystemPrompt,
      AgentPermissionEngine_GetMode(&harness->Permissions));
  AgentProvider_BuildRequest(&harness->Work, effectiveSystemPrompt, contextMessages.Data,
                             contextMessages.Count, harness->Config.Tools,
                             harness->Config.ToolCount, &request);

  for (u32 attemptIndex = 0;; attemptIndex++) {
    AgentHarnessProviderInput effective = {};
    effective.ModelInfo = run && run->HasModel ? run->ModelInfo : harness->Config.ModelInfo;
    effective.Context = request.Context;
    effective.Options = harness->Config.Stream;
    if (run && run->HasProfile) effective.Options.Profile = run->Profile;
    HttpRetryPolicy hookRetry = {};
    if (harness->Config.Hooks.BeforeProvider) {
      OwnMutableProviderInput(&harness->Work, &effective);
      if (effective.Options.Retry) {
        hookRetry = *effective.Options.Retry;
        effective.Options.Retry = &hookRetry;
      }
      SessionResult hooked = AgentHook_Failure(
          harness->Config.Hooks.BeforeProvider(harness->Config.Hooks.User, &harness->Work,
                                               turnKind, attemptIndex, &effective),
          "before-provider hook failed");
      if (!SessionResult_IsOk(hooked)) return hooked;
    }
    if (!ResolveProviderInput(&effective))
      return SessionResult_Error(
          SessionStatus::HookError,
          SVL("effective provider settings are invalid (model, token limit, or profile)"));

    StreamOptions options = effective.Options;
    options.Abort = &harness->AbortFlag;
    HttpRetryPolicy providerRetry = {};
    if (options.Retry) {
      providerRetry = *options.Retry;
      providerRetry.EventUser = harness;
      providerRetry.OnEvent = OnProviderRequestRetry;
      options.Retry = &providerRetry;
    }
    AgentProviderAttemptInput input = {};
    input.Mem = &harness->Work;
    input.ProviderImpl = harness->Config.ProviderImpl;
    input.ModelInfo = &effective.ModelInfo;
    input.Context = &effective.Context;
    input.Options = options;
    input.Sink.User = harness;
    input.Sink.OnEvent = OnProviderStreamEvent;
    input.AuthUser = harness->Config.AuthUser;
    input.ResolveAuth = harness->Config.ResolveAuth;
    AgentProviderAttemptResult attempt = AgentProvider_RunAttempt(&input);

    if (harness->Config.Hooks.AfterProvider) {
      OwnMutableMessage(&harness->Work, &attempt.Assistant);
      SessionResult hooked = AgentHook_Failure(
          harness->Config.Hooks.AfterProvider(harness->Config.Hooks.User, &harness->Work,
                                              turnKind, attemptIndex, &attempt.Assistant),
          "after-provider hook failed");
      if (!SessionResult_IsOk(hooked)) return hooked;
    }

    AgentRetryDecision decision = AgentProvider_RetryDecision(
        &harness->Config.Retry, attemptIndex, attempt.Ok, &attempt.Assistant);
    if (decision.Retry) {
      u32 attemptNumber = attemptIndex + 1;
      AgentHarness_PublishRetry(harness, AgentHarnessEventKind::TransientRetryStart,
                                RetryLayer::AssistantTurn, attemptNumber,
                                harness->Config.Retry.MaxRetries, decision.DelayMs,
                                attempt.Assistant.ErrorMessage);
      bool aborted = AgentProvider_SleepBackoff(decision.DelayMs, &harness->AbortFlag);
      AgentHarness_PublishRetry(harness, AgentHarnessEventKind::TransientRetryEnd,
                                RetryLayer::AssistantTurn, attemptNumber,
                                harness->Config.Retry.MaxRetries, decision.DelayMs,
                                attempt.Assistant.ErrorMessage);
      if (aborted)
        return SessionResult_Error(SessionStatus::Cancelled,
                                   SVL("turn cancelled during retry backoff"));
      continue;
    }

    out->Assistant = attempt.Assistant;
    out->ProviderOk = attempt.Ok;
    return SessionResult_Ok();
  }
}

// Resolves each tool call's effective arguments (BeforeTool hook), runs the
// batch (parallel by default), then appends every tool-result message in
// assistant/source order as one batch. Every prepared call always receives
// exactly one finalized result -- including a live cancellation, which still
// runs each already-dispatched call to a normal (possibly error) completion
// before this function returns -- so the harness never leaves a live tool
// call without a result before the next provider request.
SessionResult RunToolBatch(AgentHarness* harness, DurableId* ioHead, const Message* assistant,
                          AgentRunState* run, bool* outAnyTool, bool* outAllTerminate,
                          Array<Message>* outToolResults) {
  Arena* arena = &harness->Work;
  Message effectiveAssistant = *assistant;
  effectiveAssistant.Content =
      Array_Make<Content>(arena, assistant->Content.Count ? assistant->Content.Count : 1);
  SessionResult beforeToolFailure = SessionResult_Ok();
  for (u32 i = 0; i < assistant->Content.Count; i++) {
    Content content = assistant->Content.Data[i];
    if (content.Kind == ContentKind::ToolCall && !Atomic_LoadI32(&harness->AbortFlag) &&
        harness->Config.Hooks.RewriteToolArguments &&
        SessionResult_IsOk(beforeToolFailure)) {
      StrView arguments = MutableText(arena, content.ToolArgsJson);
      StrView toolName = MutableText(arena, content.ToolName);
      SessionResult hooked = AgentHook_Failure(
          harness->Config.Hooks.RewriteToolArguments(harness->Config.Hooks.User,
                                                     arena, toolName, &arguments),
          "rewrite-tool-arguments hook failed");
      if (!SessionResult_IsOk(hooked)) {
        beforeToolFailure = hooked;
      } else if (arguments.Length == 0) {
        beforeToolFailure = SessionResult_Error(
            SessionStatus::HookError,
            SVL("rewrite-tool-arguments hook produced empty arguments"));
      } else {
        content.ToolArgsJson = StrView_Dup(arena, arguments);
      }
    }
    Array_Push(&effectiveAssistant.Content, content);
  }

  AgentToolBatchPlan plan = {};
  AgentPermissionGate permissionGate = {};
  permissionGate.Engine = &harness->Permissions;
  if (run) {
    permissionGate.NewMessages = run->NewMessages.Data;
    permissionGate.NewMessageCount = run->NewMessages.Count;
  }
  permissionGate.Abort = &harness->AbortFlag;
  AgentToolBatch_Prepare(arena, harness->RuntimeTools, harness->Config.ToolCount,
                        &effectiveAssistant, harness->Config.ToolExec, &harness->AbortFlag,
                        &harness->Config.Hooks, &permissionGate, &plan);
  if (!SessionResult_IsOk(beforeToolFailure)) {
    for (u32 i = 0; i < plan.Calls.Count; i++) {
      plan.Calls.Data[i].Runnable = false;
      plan.Calls.Data[i].Result = Array_Make<Content>(arena, 1);
      Array_Push(&plan.Calls.Data[i].Result, Content_Text(beforeToolFailure.Message));
      plan.Calls.Data[i].IsError = true;
      plan.Calls.Data[i].Terminate = false;
    }
    plan.RunnableCount = 0;
  }
  if (plan.Calls.Count == 0) {
    *outAnyTool = false;
    *outAllTerminate = true;
    return SessionResult_Ok();
  }

  for (u32 i = 0; i < plan.Calls.Count; i++) {
    const Content* call = plan.Calls.Data[i].Call;
    AgentHarness_PublishToolStart(harness, call->ToolId, call->ToolName,
                                  plan.Calls.Data[i].EffectiveArgumentsJson);
  }

  AgentToolBatchResult results = {};
  AgentToolDispatchGate dispatch = {};
  dispatch.User = harness;
  dispatch.TryStart = TryStartTool;
  AgentToolEventSink events = ToolEvents(harness);
  AgentToolBatch_Run(arena, &plan, harness->Config.MaxParallelTools,
                     &harness->Config.Hooks, &dispatch, &events, &results);

  Array<SessionRecord> messages =
      Array_Make<SessionRecord>(arena, results.Calls.Count ? results.Calls.Count : 1);
  DurableId head = *ioHead;
  for (u32 i = 0; i < results.Calls.Count; i++) {
    SessionRecord message =
        NewMessageRecord(head, AgentToolResult_Materialize(&results.Calls.Data[i]));
    Array_Push(&messages, message);
    head = message.EntryId;
  }
  SessionResult committed = AgentHarness_Append(harness, messages.Data, messages.Count);
  if (SessionResult_IsOk(committed)) {
    *ioHead = head;
    *outAnyTool = results.AnyTool;
    *outAllTerminate = results.AllTerminate;
    AgentRunState_Record(run, messages.Data, messages.Count);
    if (outToolResults) {
      for (u32 i = 0; i < messages.Count; i++)
        Array_Push(outToolResults, messages.Data[i].MessageEntry.Value);
    }
  }
  if (!SessionResult_IsOk(committed)) return committed;
  if (!SessionResult_IsOk(beforeToolFailure)) return beforeToolFailure;
  return Atomic_LoadI32(&harness->AbortFlag)
             ? SessionResult_Error(SessionStatus::Cancelled,
                                   SVL("run cancelled during tool execution"))
             : committed;
}

// Only Text blocks form the summary, as in pi: reasoning is the model's
// working, not part of the answer.
StrView ExtractSummaryText(Arena* arena, const Message* assistant) {
  StrBuf text = StrBuf_Make(arena, 256);
  for (u32 i = 0; i < assistant->Content.Count; i++) {
    const Content* content = &assistant->Content.Data[i];
    if (content->Kind == ContentKind::Text && content->Text.Length) {
      if (text.Length) StrBuf_AppendChar(&text, '\n');
      StrBuf_Append(&text, content->Text);
    }
  }
  return StrBuf_View(&text);
}

struct CompactionTurnResult {
  AgentTurnOutcome Outcome;
  StrView Error;
  DurableId NewHead;
};

// Runs one compaction turn over the inclusive branch span [base..through] and
// appends the resulting Compaction entry chained from `head`. Publishes only
// turn-level events, so an inline compaction never emits a nested agent
// lifecycle inside the enclosing run.
SessionResult PerformCompaction(AgentHarness* harness, DurableId head,
                                DurableId baseEntryId, DurableId throughEntryId,
                                StrView instructions,
                                CompactionTurnResult* out) {
  AgentHarness_PublishTurnStart(harness, AgentTurnKind::Compaction);

  StrView summarizeText =
      SVL("Summarize the conversation for future continuation. Preserve goals, "
          "decisions, constraints, unresolved work, and important tool results.");
  if (instructions.Length) {
    // pi's separator, byte for byte, so a summary written against pi's
    // instructions reads the same here.
    StrBuf directed =
        StrBuf_Make(&harness->Work, summarizeText.Length + instructions.Length + 24);
    StrBuf_Append(&directed, summarizeText);
    StrBuf_AppendCStr(&directed, "\n\nAdditional focus: ");
    StrBuf_Append(&directed, instructions);
    summarizeText = StrBuf_View(&directed);
  }
  Message summarizePrompt = Message_User(&harness->Work, summarizeText);

  // The summarizer sees only the span being folded, as the model sees it now.
  // The retained tail stays verbatim after the summary, so including it would
  // duplicate it and spend the very headroom compaction exists to recover.
  AgentRunState span = {};
  span.HasContext = true;
  SessionReduction reduction = AgentHarness_ReduceSnapshot(harness);
  SessionResult result = SessionReducer_ContextThrough(&harness->Work, &reduction, head,
                                                       throughEntryId, &span.Context);
  TurnRunResult turn = {};
  if (SessionResult_IsOk(result))
    result = RunAssistantTurn(harness, AgentTurnKind::Compaction, head, &summarizePrompt,
                              &span, &turn);

  AgentTurnOutcome outcome = AgentTurnOutcome::Succeeded;
  StrView error = {};

  if (SessionResult_IsOk(result)) {
    bool success = turn.ProviderOk && turn.Assistant.Stop != StopReason::Error &&
                   turn.Assistant.Stop != StopReason::Aborted;
    StrView summaryText =
        success ? ExtractSummaryText(&harness->Work, &turn.Assistant) : StrView{};
    if (!success || summaryText.Length == 0) {
      bool aborted = turn.Assistant.Stop == StopReason::Aborted;
      error = summaryText.Length == 0 && success
                  ? SVL("compaction provider returned an empty summary")
                  : turn.Assistant.ErrorMessage;
      result = SessionResult_Error(
          aborted ? SessionStatus::Cancelled : SessionStatus::ProviderError, error);
    } else {
      // The span being folded away is the last place these paths appear, so
      // they are collected before the summary is sealed and carried on the
      // record for the next compaction to reseed from.
      AgentFileLists files = {};
      reduction = AgentHarness_ReduceSnapshot(harness);
      Array<const SessionRecord*> branch = {};
      if (SessionResult_IsOk(
              SessionReducer_Ancestry(&reduction, &harness->Work, head, &branch))) {
        AgentFileOps ops = {};
        AgentCompaction_CollectFileOps(&harness->Work, branch.Data, branch.Count,
                                       throughEntryId, &ops);
        AgentCompaction_BuildFileLists(&harness->Work, &ops, &files);
      }

      StrBuf wrapped = StrBuf_Make(&harness->Work, summaryText.Length + 48);
      StrBuf_AppendCStr(&wrapped, "Summary of previous conversation:\n");
      StrBuf_Append(&wrapped, summaryText);
      AgentCompaction_AppendFileSections(&wrapped, &files);
      Message summary = Message_User(&harness->Work, StrBuf_View(&wrapped));

      SessionRecord compactionRecord = {};
      compactionRecord.EntryId = DurableId_Generate();
      compactionRecord.Kind = SessionRecordKind::Compaction;
      compactionRecord.CompactionEntry.ParentId = head;
      compactionRecord.CompactionEntry.BaseEntryId = baseEntryId;
      compactionRecord.CompactionEntry.ThroughEntryId = throughEntryId;
      compactionRecord.CompactionEntry.SourceHeadId = head;
      compactionRecord.CompactionEntry.Summary = summary;
      compactionRecord.CompactionEntry.HasUsage = true;
      compactionRecord.CompactionEntry.Use = turn.Assistant.Use;
      compactionRecord.CompactionEntry.ReadFiles = files.ReadFiles;
      compactionRecord.CompactionEntry.ModifiedFiles = files.ModifiedFiles;
      result = AgentHarness_Append(harness, &compactionRecord, 1, true);
      if (SessionResult_IsOk(result)) out->NewHead = compactionRecord.EntryId;
    }
  }

  if (!SessionResult_IsOk(result)) {
    outcome = result.Status == SessionStatus::Cancelled ? AgentTurnOutcome::Aborted
                                                        : AgentTurnOutcome::Failed;
    if (!error.Length) error = result.Message;
  }
  AgentHarness_PublishTurnEnd(harness, AgentTurnKind::Compaction, outcome, error);
  out->Outcome = outcome;
  out->Error = error;
  return result;
}

// The window the next provider request will actually be sized against: the
// explicitly selected tier when the model exposes one, else its fixed window.
// A run's PrepareNextTurn overrides are what the request will carry, so they
// take precedence over the config here too.
u32 ActiveContextWindow(const AgentHarness* harness, const AgentRunState* run) {
  u32 window = run && run->HasProfile ? run->Profile.ContextWindow
                                      : harness->Config.Stream.Profile.ContextWindow;
  if (window) return window;
  return run && run->HasModel ? run->ModelInfo.ContextWindow
                              : harness->Config.ModelInfo.ContextWindow;
}

// Decides whether the next provider request for `head` would eat into the
// window's reserved headroom, and if so which branch span to fold away.
// Measurement materializes the branch context once, the same O(branch) cost the
// turn itself pays -- or reads a live context override directly, since that is
// what the request will actually carry. The cut point is always selected from
// the durable branch, which is what compaction folds.
bool ShouldAutoCompact(AgentHarness* harness, DurableId head, const AgentRunState* run,
                       AgentCompactionCut* outCut) {
  const AgentCompactionPolicy* policy = &harness->Config.Compaction;
  u32 window = ActiveContextWindow(harness, run);
  if (!AgentCompactionPolicy_IsValid(policy) || !window) return false;

  SessionReduction reduction = AgentHarness_ReduceSnapshot(harness);
  u32 tokens = 0;
  if (run && run->HasContext) {
    // A hook-supplied context may keep an assistant message while dropping the
    // history it was billed against, so the reported-usage anchor's invariant
    // ("this usage paid for everything before it") no longer holds. Only the
    // byte estimate is meaningful for an array the harness did not assemble.
    tokens = AgentCompaction_EstimateContextTokens(run->Context.Data, run->Context.Count);
  } else {
    Array<Message> messages = {};
    if (!SessionResult_IsOk(
            SessionReducer_Context(&harness->Work, &reduction, head, &messages)))
      return false;
    tokens = AgentCompaction_ContextTokens(messages.Data, messages.Count);
  }
  if (!AgentCompaction_ShouldCompact(policy, window, tokens)) return false;

  Array<const SessionRecord*> branch = {};
  if (!SessionResult_IsOk(
          SessionReducer_Ancestry(&reduction, &harness->Work, head, &branch)))
    return false;
  return AgentCompaction_SelectCutPoint(branch.Data, branch.Count,
                                        policy->KeepRecentTokens, outCut);
}

// Materializes the state pi hands to prepareNextTurn / shouldStopAfterTurn.
// Only built when one of them is installed, since the context array costs
// O(branch) to materialize.
SessionResult BuildTurnContext(AgentHarness* harness, DurableId head, const Message* assistant,
                               Array<Message> toolResults, const AgentRunState* run,
                               AgentTurnContext* out) {
  SessionReduction reduction = AgentHarness_ReduceSnapshot(harness);
  Array<Message> context = {};
  SessionResult result = SessionReducer_Context(&harness->Work, &reduction, head, &context);
  if (!SessionResult_IsOk(result)) return result;
  out->Turn = AgentTurnKind::Run;
  out->HeadId = head;
  out->Assistant = assistant;
  out->ToolResults = toolResults;
  out->Context = context;
  if (run) out->NewMessages = run->NewMessages;
  return SessionResult_Ok();
}

// The turn budget is spent. Ask whether to keep going, and grant a fresh budget
// when the user says yes.
//
// Deliberately called with no harness lock held: this blocks until the user
// answers, and blocking under StateLock or WatcherLock would stall every
// watcher and wedge the process. Allocates from Work, which the caller has just
// reset, so the question and answer cost one turn's scratch and no more.
bool AskToContinuePastTurnLimit(AgentHarness* harness, u32 turnsUsed) {
  StrBuf question = StrBuf_Make(&harness->Work, 160);
  StrBuf_AppendCStr(&question, "The agent has run ");
  StrBuf_AppendU64(&question, turnsUsed);
  StrBuf_AppendCStr(&question,
                    " turns in a row without input. Keep going?");

  StrView choices[] = {SVL("Continue"), SVL("Stop")};
  AgentAskRequest request = {};
  request.Question = StrBuf_View(&question);
  request.Choices = choices;
  request.ChoiceCount = 2;

  AgentAskResponse response =
      AgentAskUser_Invoke(&harness->Config.AskUser, &harness->Work, &request);
  return response.Status == AgentAskStatus::Answered && response.ChoiceIndex == 0;
}

SessionResult ContinueRun(AgentHarness* harness, DurableId* ioHead, AgentRunState* run) {
  for (;;) {
    if (Atomic_LoadI32(&harness->AbortFlag))
      return SessionResult_Error(SessionStatus::Cancelled, SVL("run cancelled"));

    // Nothing this loop allocates from Work survives an iteration: every
    // message it produces is appended first, and AgentHarness_Append rewrites
    // each record with a State-owned clone. Without this a run that takes N
    // turns stacks N materialized contexts and N request bodies, which is what
    // took one real session's Work arena to 62 MB. Anything that does span
    // turns lives in RunMem or the run's replaceable override owners instead.
    ArenaReset(&harness->Work);
    ArenaTrim(&harness->Work, WORK_ARENA_KEEP_BYTES);

    // Runaway guard. Checked here, before anything for this turn is built, so a
    // turn that would exceed the budget is never started and no provider
    // request is paid for only to be discarded.
    if (run && AgentTurnLimit_Reached(&harness->Config.TurnLimit,
                                      run->AutonomousTurns - run->GrantedTurns)) {
      u32 spent = run->AutonomousTurns;
      if (!AskToContinuePastTurnLimit(harness, spent)) {
        StrBuf notice = StrBuf_Make(&harness->Work, 96);
        StrBuf_AppendCStr(&notice, "stopped after ");
        StrBuf_AppendU64(&notice, spent);
        StrBuf_AppendCStr(&notice, " turns without input");
        // A graceful stop, not a failure: the branch is intact and an ordinary
        // follow-up prompt resumes from exactly here. Returning Ok *with* a
        // message is what carries the reason to the UI --
        // CompleteAgentTurnLocked publishes agent_end with it and copies it out
        // of Work first, so nothing here has to publish or own the string.
        return SessionResult{SessionStatus::Ok, StrBuf_View(&notice), 0};
      }
      run->GrantedTurns = spent;
    }
    if (run) run->AutonomousTurns++;

    // Automatic compaction runs before the next provider request, at most once
    // per iteration. If it succeeds but the context still exceeds the budget we
    // continue anyway and let the provider decide, rather than loop.
    AgentCompactionCut cut = {};
    if (ShouldAutoCompact(harness, *ioHead, run, &cut)) {
      if (harness->Config.Hooks.BeforeCompaction) {
        SessionResult hooked = AgentHook_Failure(
            harness->Config.Hooks.BeforeCompaction(harness->Config.Hooks.User,
                                                   &harness->Work, *ioHead),
            "before-compaction hook failed");
        if (!SessionResult_IsOk(hooked)) return hooked;
      }
      CompactionTurnResult compaction = {};
      // Automatic compaction never inherits `/compact` instructions; pi
      // discards them after the invocation that supplied them.
      SessionResult compacted = PerformCompaction(harness, *ioHead, cut.BaseEntryId,
                                                  cut.ThroughEntryId, StrView{},
                                                  &compaction);
      if (!SessionResult_IsOk(compacted)) {
        if (compacted.Status == SessionStatus::Cancelled) return compacted;
        StrBuf message = StrBuf_Make(&harness->Work, compacted.Message.Length + 40);
        StrBuf_AppendCStr(&message, "automatic compaction failed");
        if (compacted.Message.Length) {
          StrBuf_AppendCStr(&message, ": ");
          StrBuf_Append(&message, compacted.Message);
        }
        return SessionResult_Error(compacted.Status, StrBuf_View(&message));
      }
      *ioHead = compaction.NewHead;
      // Compaction rewrote the branch the override was derived from, so the
      // override is stale and there is no sound way to rebase it: fall back to
      // the compacted branch. Because ShouldAutoCompact sizes against the
      // override itself, this only happens when the override genuinely
      // outgrew the window, never because the branch behind it did.
      if (run) AgentRunState_DropContext(run);
    }

    AgentHarness_PublishTurnStart(harness, AgentTurnKind::Run);
    TurnRunResult turn = {};
    SessionResult generated =
        RunAssistantTurn(harness, AgentTurnKind::Run, *ioHead, nullptr, run, &turn);
    if (!SessionResult_IsOk(generated)) {
      bool cancelled = generated.Status == SessionStatus::Cancelled;
      AgentHarness_PublishTurnEnd(harness, AgentTurnKind::Run,
                                 cancelled ? AgentTurnOutcome::Aborted : AgentTurnOutcome::Failed,
                                 generated.Message);
      return generated;
    }

    // message_end: append exactly one assistant message, regardless of stop
    // reason (Stop/ToolUse/Error/Aborted all become a durable message; only
    // the *next* provider request is what an error/abort short-circuits).
    SessionRecord assistantRecord = NewMessageRecord(*ioHead, turn.Assistant);
    SessionResult committed = AgentHarness_Append(harness, &assistantRecord, 1);
    if (!SessionResult_IsOk(committed)) {
      AgentHarness_PublishTurnEnd(harness, AgentTurnKind::Run, AgentTurnOutcome::Failed,
                                 committed.Message);
      return committed;
    }
    *ioHead = assistantRecord.EntryId;
    AgentRunState_Record(run, &assistantRecord, 1);

    if (!turn.ProviderOk || turn.Assistant.Stop == StopReason::Error ||
        turn.Assistant.Stop == StopReason::Aborted) {
      bool aborted = turn.Assistant.Stop == StopReason::Aborted;
      AgentHarness_PublishTurnEnd(harness, AgentTurnKind::Run,
                                 aborted ? AgentTurnOutcome::Aborted : AgentTurnOutcome::Failed,
                                 turn.Assistant.ErrorMessage);
      return SessionResult_Error(aborted ? SessionStatus::Cancelled : SessionStatus::ProviderError,
                                 turn.Assistant.ErrorMessage);
    }

    // Truncation drops the tool call the model was mid-way through emitting:
    // the provider never closes that block, so the batch below would see no
    // tools and end the run as though the model had finished.
    if (turn.Assistant.Stop == StopReason::Length) {
      StrView truncated =
          SVL("assistant response hit the output token limit before completing");
      AgentHarness_PublishTurnEnd(harness, AgentTurnKind::Run, AgentTurnOutcome::Failed,
                                 truncated);
      return SessionResult_Error(SessionStatus::ProviderError, truncated);
    }

    bool anyTool = false;
    bool allTerminate = true;
    Array<Message> toolResults = Array_Make<Message>(&harness->Work, 4);
    SessionResult tools = RunToolBatch(harness, ioHead, &turn.Assistant, run, &anyTool,
                                       &allTerminate, &toolResults);
    if (!SessionResult_IsOk(tools)) {
      AgentHarness_PublishTurnEnd(harness, AgentTurnKind::Run,
                                 tools.Status == SessionStatus::Cancelled
                                     ? AgentTurnOutcome::Aborted
                                     : AgentTurnOutcome::Failed,
                                 tools.Message);
      return tools;
    }
    AgentHarness_PublishTurnEnd(harness, AgentTurnKind::Run, AgentTurnOutcome::Succeeded, StrView{});

    // pi's prepareNextTurn then shouldStopAfterTurn: both observe the finished
    // turn after turn_end and before either steering source is polled.
    if (harness->Config.Hooks.PrepareNextTurn || harness->Config.Hooks.ShouldStopAfterTurn) {
      AgentTurnContext turnContext = {};
      SessionResult built = BuildTurnContext(harness, *ioHead, &assistantRecord.MessageEntry.Value,
                                             toolResults, run, &turnContext);
      if (!SessionResult_IsOk(built)) return built;

      if (harness->Config.Hooks.PrepareNextTurn) {
        OwnMutableTurnContext(&harness->Work, &turnContext);
        AgentNextTurnUpdate update = {};
        // Construction scratch only; Apply owns the accepted fields before
        // Work is recycled, and abandoned hook allocations die with this turn.
        SessionResult hooked = AgentHook_Failure(
            harness->Config.Hooks.PrepareNextTurn(harness->Config.Hooks.User, &harness->Work,
                                                  &turnContext, &update),
            "prepare-next-turn hook failed");
        if (!SessionResult_IsOk(hooked)) return hooked;
        SessionResult applied = AgentRunState_Apply(run, &update);
        if (!SessionResult_IsOk(applied)) return applied;
        // pi hands shouldStopAfterTurn the already-updated context, so a stop
        // decision sees what the next request would actually carry.
        if (run && run->HasContext) turnContext.Context = run->Context;
      }

      if (harness->Config.Hooks.ShouldStopAfterTurn) {
        OwnMutableTurnContext(&harness->Work, &turnContext);
        bool stop = false;
        SessionResult hooked = AgentHook_Failure(
            harness->Config.Hooks.ShouldStopAfterTurn(harness->Config.Hooks.User, &harness->Work,
                                                      &turnContext, &stop),
            "should-stop-after-turn hook failed");
        if (!SessionResult_IsOk(hooked)) return hooked;
        // A graceful stop: the run ends here without polling either queue and
        // without starting another provider request.
        if (stop) return SessionResult_Ok();
      }
    }

    // Steering folds onto the tree right after this tool-result batch, before
    // any further provider request (whether that request continues this
    // turn or starts the next one).
    u32 steerCount = 0;
    SessionResult steered = FlushSteering(harness, ioHead, run, &steerCount);
    if (!SessionResult_IsOk(steered)) return steered;
    // Steering and follow-up are the only user-driven inputs mid-run, so they
    // are the only things that refill the budget. Resetting on any appended
    // message instead would make the cap unreachable, since every turn appends.
    if (steerCount && run) {
      run->AutonomousTurns = 0;
      run->GrantedTurns = 0;
    }
    if (AgentCheckpoint_ContinueTurn(anyTool, allTerminate, steerCount)) continue;

    u32 followUpCount = 0;
    SessionResult followed = FlushFollowUp(harness, ioHead, run, &followUpCount);
    if (!SessionResult_IsOk(followed)) return followed;
    if (followUpCount && run) {
      run->AutonomousTurns = 0;
      run->GrantedTurns = 0;
    }
    if (AgentCheckpoint_ContinueRun(followUpCount)) continue;

    return SessionResult_Ok();
  }
}

}  // namespace

bool AgentDriver_ReviewPermission(
    void* user, Arena* arena, const AgentPermissionReviewRequest* request,
    volatile i32* abort, AgentPermissionReviewResult* out) {
  if (out) {
    *out = {};
  }
  AgentHarness* harness = (AgentHarness*)user;
  if (!harness || !arena || !request || !out) return false;
  StrView reviewerModel = harness->Config.PermissionReviewerModel;
  AgentHarness_PublishPermissionReviewStart(harness, request->ToolName,
                                            reviewerModel);
  const AgentPermissionReviewer* reviewer = &harness->Config.PermissionReviewer;
  bool ok = reviewer->Review &&
            reviewer->Review(reviewer->User, arena, request, abort, out);
  AgentPermissionReviewOutcome outcome = AgentPermissionReviewOutcome::Ask;
  if (!ok) {
    out->Decision = AgentPermissionReviewDecision::Ask;
    outcome = AgentPermissionReviewOutcome::Unavailable;
    if (!out->Rationale.Length)
      out->Rationale = SVL("Automatic permission review was unavailable or invalid.");
  } else if (out->Decision == AgentPermissionReviewDecision::Allow) {
    outcome = AgentPermissionReviewOutcome::Allow;
  }
  AgentHarness_PublishPermissionReviewEnd(
      harness, request->ToolName, out->Model.Length ? out->Model : reviewerModel,
      outcome, out->Rationale);
  return ok;
}

SessionResult AgentDriver_Run(AgentHarness* harness, StrView prompt) {
  if (prompt.Length == 0)
    return SessionResult_Error(SessionStatus::InvalidArgument,
                               SVL("run prompt must not be empty"));
  AgentHarness_PublishAgentStart(harness, AgentTurnKind::Run);

  DurableId head = AgentHarness_ReduceSnapshot(harness).HeadId;
  AgentRunState run = {};
  run.NewMessages = Array_Make<Message>(&harness->RunMem, 8);
  u32 nextTurnCount = 0;
  SessionResult result =
      FlushQueueIntoRun(harness, &harness->Queues.NextTurn, &head, &run, &nextTurnCount);

  if (SessionResult_IsOk(result)) {
    Message accepted = Message_User(&harness->Work, StrView_Dup(&harness->Work, prompt));
    if (harness->Config.Hooks.BeforeRun) {
      result = AgentHook_Failure(
          harness->Config.Hooks.BeforeRun(harness->Config.Hooks.User, &harness->Work, &accepted),
          "before-run hook failed");
    }
    if (SessionResult_IsOk(result)) {
      SessionRecord userRecord = NewMessageRecord(head, accepted);
      result = AgentHarness_Append(harness, &userRecord, 1);
      if (SessionResult_IsOk(result)) {
        head = userRecord.EntryId;
        AgentRunState_Record(&run, &userRecord, 1);
      }
    }
  }

  // pi polls steering once before the first turn, picking up anything the user
  // typed while the previous run was still finishing.
  if (SessionResult_IsOk(result)) {
    u32 steerCount = 0;
    result = FlushSteering(harness, &head, &run, &steerCount);
  }

  if (SessionResult_IsOk(result)) result = ContinueRun(harness, &head, &run);

  AgentTurnOutcome outcome = AgentTurnOutcome::Succeeded;
  StrView error = {};
  if (!SessionResult_IsOk(result)) {
    outcome =
        result.Status == SessionStatus::Cancelled ? AgentTurnOutcome::Aborted : AgentTurnOutcome::Failed;
    error = result.Message;
  }
  if (harness->Config.Hooks.AfterTurn) {
    SessionResult afterTurn = AgentHook_Failure(
        harness->Config.Hooks.AfterTurn(harness->Config.Hooks.User, &harness->Work,
                                        AgentTurnKind::Run, outcome,
                                        MutableText(&harness->Work, error)),
        "after-turn hook failed");
    if (SessionResult_IsOk(result) && !SessionResult_IsOk(afterTurn)) {
      result = afterTurn;
      outcome = AgentTurnOutcome::Failed;
      error = result.Message;
    }
  }
  // Any hook/provider error (or successful stop notice) can borrow an override.
  // CompleteAgentTurnLocked copies from Work only after the driver returns.
  if (result.Message.Length && AgentRunState_OwnsText(&run, result.Message))
    result.Message = StrView_Dup(&harness->Work, result.Message);
  AgentRunState_DropContext(&run);
  ArenaRelease(&run.ModelMem);
  return result;
}

SessionResult AgentDriver_Compact(AgentHarness* harness, StrView instructions) {
  AgentHarness_PublishAgentStart(harness, AgentTurnKind::Compaction);
  SessionReduction initial = AgentHarness_ReduceSnapshot(harness);
  DurableId head = initial.HeadId;

  SessionResult result = SessionResult_Ok();
  if (harness->Config.Hooks.BeforeCompaction) {
    result = AgentHook_Failure(
        harness->Config.Hooks.BeforeCompaction(harness->Config.Hooks.User, &harness->Work, head),
        "before-compaction hook failed");
  }

  AgentTurnOutcome outcome = AgentTurnOutcome::Failed;
  StrView error = {};

  if (SessionResult_IsOk(result)) {
    // Manual compaction always folds the whole branch: base is the session
    // root, which is a tree node on every branch.
    CompactionTurnResult compaction = {};
    result = PerformCompaction(
        harness, head, initial.RecordCount ? initial.Records[0].EntryId : DurableId{},
        head, instructions, &compaction);
    outcome = compaction.Outcome;
    error = compaction.Error;
  } else {
    error = result.Message;
  }

  if (harness->Config.Hooks.AfterTurn) {
    SessionResult afterTurn = AgentHook_Failure(
        harness->Config.Hooks.AfterTurn(harness->Config.Hooks.User, &harness->Work,
                                        AgentTurnKind::Compaction, outcome,
                                        MutableText(&harness->Work, error)),
        "after-turn hook failed");
    if (SessionResult_IsOk(result) && !SessionResult_IsOk(afterTurn)) {
      result = afterTurn;
      outcome = AgentTurnOutcome::Failed;
      error = result.Message;
    }
  }
  return result;
}
