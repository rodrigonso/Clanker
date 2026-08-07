#include <string.h>

#include "agent/driver.h"
#include "agent/harness.h"
#include "base/arena.h"
#include "base/thread.h"
#include "check.h"
#include "script_provider.h"
#include "net/http.h"
#include "session/memory_repository.h"
#include "session/metadata.h"

namespace {

SessionResult CreateSession(SessionRepository* repository, Arena* arena, SessionLog* out) {
  SessionCreateOptions options = {};
  options.ProjectKey = SVL("harness-tests");
  options.WorkingDirectory = SVL("C:\\work\\clank");
  options.Label = SVL("harness");
  SessionInfo info = {};
  return repository->Create(repository->Ctx, arena, &options, out, &info);
}

bool EchoExec(void* context, Arena* arena, StrView id, const ToolArgs* args, volatile i32* abort,
             const ToolUpdateSink* updates, ToolResult* out, StrBuf* error) {
  (void)id;
  (void)abort;
  (void)updates;
  (void)error;
  if (context) (*(u32*)context)++;
  StrBuf text = StrBuf_Make(arena, 32);
  StrBuf_AppendCStr(&text, "echo:");
  StrBuf_Append(&text, args->Json);
  out->Content = Array_Make<Content>(arena, 1);
  Array_Push(&out->Content, Content_Text(StrBuf_View(&text)));
  return true;
}

bool SteerExec(void* context, Arena* arena, StrView id, const ToolArgs* args, volatile i32* abort,
              const ToolUpdateSink* updates, ToolResult* out, StrBuf* error) {
  (void)id;
  (void)args;
  (void)abort;
  (void)updates;
  AgentHarness* harness = (AgentHarness*)context;
  SessionResult queued = AgentHarness_QueueSteering(harness, SVL("steered durably"));
  if (!SessionResult_IsOk(queued)) {
    StrBuf_Append(error, queued.Message);
    return false;
  }
  out->Content = Array_Make<Content>(arena, 1);
  Array_Push(&out->Content, Content_Text(SVL("queued")));
  return true;
}

bool CountExec(void* context, Arena* arena, StrView id, const ToolArgs* args, volatile i32* abort,
              const ToolUpdateSink* updates, ToolResult* out, StrBuf* error) {
  (void)id;
  (void)args;
  (void)abort;
  (void)updates;
  (void)error;
  (*(u32*)context)++;
  out->Content = Array_Make<Content>(arena, 1);
  Array_Push(&out->Content, Content_Text(SVL("ran")));
  return true;
}

struct ExecutionCounter {
  Mutex Lock;
  u32 Value;
};

u32 ExecutionCounter_Read(ExecutionCounter* counter) {
  ScopedLock lock(&counter->Lock);
  return counter->Value;
}

bool DelayExec(void* context, Arena* arena, StrView id, const ToolArgs* args, volatile i32* abort,
              const ToolUpdateSink* updates, ToolResult* out, StrBuf* error) {
  (void)id;
  (void)abort;
  (void)error;
  ExecutionCounter* executions = (ExecutionCounter*)context;
  {
    ScopedLock lock(&executions->Lock);
    executions->Value++;
  }
  StrView label = ToolArgs_GetStr(args, "label");
  StrBuf updateText = StrBuf_Make(arena, label.Length + 16);
  StrBuf_AppendCStr(&updateText, "started:");
  StrBuf_Append(&updateText, label);
  ToolResult partial = {};
  partial.Content = Array_Make<Content>(arena, 1);
  Array_Push(&partial.Content, Content_Text(StrBuf_View(&updateText)));
  ToolUpdateSink_Emit(updates, &partial);
  Sleep_Ms((u32)ToolArgs_GetInt(args, "delay", 0));
  out->Content = Array_Make<Content>(arena, 1);
  Array_Push(&out->Content, Content_Text(label));
  return true;
}

struct HarnessHookState {
  u32 BeforeRunCount;
  u32 BeforeContextCount;
  u32 BeforeProviderCount;
  u32 AfterProviderCount;
  u32 BeforeToolCount;
  u32 AfterTurnCount;
  u32 FirstContextCount;
  u32 LastContextCount;
};

SessionResult HookBeforeRun(void* context, Arena* arena, Message* prompt) {
  HarnessHookState* state = (HarnessHookState*)context;
  state->BeforeRunCount++;
  prompt->Content.Data[0].Text = StrView_Dup(arena, SVL("hooked durable prompt"));
  return SessionResult_Ok();
}

SessionResult FailAfterTurn(void*, Arena*, AgentTurnKind, AgentTurnOutcome,
                            StrView) {
  return SessionResult_Error(SessionStatus::InvalidArgument,
                             SVL("after-turn hook failed"));
}

SessionResult HookBeforeContext(void* context, Arena* arena, AgentTurnKind turn, DurableId headId,
                                Array<Message>* messages) {
  (void)turn;
  (void)headId;
  HarnessHookState* state = (HarnessHookState*)context;
  state->BeforeContextCount++;
  Array_Push(messages, Message_User(arena, SVL("request-only hook context")));
  if (state->BeforeContextCount == 1) state->FirstContextCount = messages->Count;
  state->LastContextCount = messages->Count;
  return SessionResult_Ok();
}

SessionResult HookBeforeProvider(void* context, Arena* arena, AgentTurnKind turn, u32 attemptIndex,
                                 AgentHarnessProviderInput* input) {
  (void)turn;
  (void)attemptIndex;
  HarnessHookState* state = (HarnessHookState*)context;
  state->BeforeProviderCount++;
  input->ModelInfo.Provider = StrView_Dup(arena, SVL("hook-provider"));
  input->ModelInfo.Id = StrView_Dup(arena, SVL("hook-model"));
  input->Options.MaxTokens = 321;
  return SessionResult_Ok();
}

SessionResult HookAfterProvider(void* context, Arena* arena, AgentTurnKind turn, u32 attemptIndex,
                                Message* assistant) {
  (void)arena;
  (void)turn;
  (void)attemptIndex;
  (void)assistant;
  ((HarnessHookState*)context)->AfterProviderCount++;
  return SessionResult_Ok();
}

SessionResult HookBeforeTool(void* context, Arena* arena, StrView toolName, StrView* argumentsJson) {
  (void)toolName;
  HarnessHookState* state = (HarnessHookState*)context;
  state->BeforeToolCount++;
  *argumentsJson = StrView_Dup(arena, SVL("{\"hooked\":true}"));
  return SessionResult_Ok();
}

SessionResult HookAfterTurn(void* context, Arena* arena, AgentTurnKind turn, AgentTurnOutcome outcome,
                            StrView error) {
  (void)arena;
  (void)turn;
  (void)outcome;
  (void)error;
  ((HarnessHookState*)context)->AfterTurnCount++;
  return SessionResult_Ok();
}

SessionResult FailBeforeTool(void* context, Arena* arena, StrView toolName, StrView* argumentsJson) {
  (void)arena;
  (void)toolName;
  (void)argumentsJson;
  (*(u32*)context)++;
  return SessionResult_Error(SessionStatus::InvalidArgument, SVL("tool hook failed"));
}

struct BlockingBeforeToolState {
  volatile i32 Entered;
  volatile i32 Release;
};

SessionResult BlockingBeforeTool(void* context, Arena* arena, StrView toolName,
                                 StrView* argumentsJson) {
  (void)arena;
  (void)toolName;
  (void)argumentsJson;
  BlockingBeforeToolState* state = (BlockingBeforeToolState*)context;
  state->Entered = 1;
  while (!state->Release) Sleep_Ms(1);
  return SessionResult_Ok();
}

SessionResult BlockingAfterTurn(void* context, Arena* arena,
                                AgentTurnKind turn,
                                AgentTurnOutcome outcome, StrView error) {
  (void)arena;
  (void)turn;
  (void)outcome;
  (void)error;
  BlockingBeforeToolState* state = (BlockingBeforeToolState*)context;
  state->Entered = 1;
  while (!state->Release) Sleep_Ms(1);
  return SessionResult_Ok();
}

// Per-turn hook coverage: pi's prepareNextTurn / shouldStopAfterTurn /
// getSteeringMessages / getFollowUpMessages / convertToLlm.
struct TurnHookState {
  u32 PrepareCount;
  u32 ShouldStopCount;
  u32 SteeringCount;
  u32 FollowUpCount;
  u32 ConvertCount;
  u32 BeforeContextCount;
  u32 StopAfterTurn;   // 1-based turn that requests the stop; 0 never stops
  u32 SteeringBudget;  // how many polls actually yield a message
  u32 FollowUpBudget;
  bool OverrideNextTurn;
  bool OverrideContext;
  bool KeepAssistantInOverride;
  u32 StopSeenContext[8];
  u32 StopSeenCount;
  // Observations from the last invocation of each hook.
  u32 LastToolResults;
  u32 LastContextCount;
  u32 LastNewMessages;
  bool LastAssistantSeen;
  u32 BeforeContextLastCount;
  u32 ConvertLastCount;
  bool ProviderSawOverride[8];
  ModelEffort ProviderEffort[8];
  u32 ProviderMessageCount[8];
  u32 ProviderTurnCount;
};

SessionResult HookCountBeforeContext(void* context, Arena* arena, AgentTurnKind turn,
                                     DurableId headId, Array<Message>* messages) {
  (void)turn;
  (void)headId;
  TurnHookState* state = (TurnHookState*)context;
  state->BeforeContextCount++;
  Array_Push(messages, Message_User(arena, SVL("before-context marker")));
  state->BeforeContextLastCount = messages->Count;
  return SessionResult_Ok();
}

SessionResult HookConvertToLlm(void* context, Arena* arena, AgentTurnKind turn, DurableId headId,
                               Array<Message>* messages) {
  (void)turn;
  (void)headId;
  TurnHookState* state = (TurnHookState*)context;
  state->ConvertCount++;
  state->ConvertLastCount = messages->Count;
  Array_Push(messages, Message_User(arena, SVL("convert-to-llm marker")));
  return SessionResult_Ok();
}

SessionResult HookObserveProvider(void* context, Arena* arena, AgentTurnKind turn,
                                  u32 attemptIndex, AgentHarnessProviderInput* input) {
  (void)arena;
  (void)turn;
  TurnHookState* state = (TurnHookState*)context;
  if (attemptIndex != 0 || state->ProviderTurnCount >= 8) return SessionResult_Ok();
  u32 index = state->ProviderTurnCount++;
  state->ProviderSawOverride[index] = StrView_EqCStr(input->ModelInfo.Id, "override-model");
  state->ProviderEffort[index] = input->Options.Profile.Effort;
  state->ProviderMessageCount[index] = input->Context.MessageCount;
  return SessionResult_Ok();
}

SessionResult HookPrepareNextTurn(void* context, Arena* arena, const AgentTurnContext* turn,
                                  AgentNextTurnUpdate* out) {
  TurnHookState* state = (TurnHookState*)context;
  state->PrepareCount++;
  state->LastToolResults = turn->ToolResults.Count;
  state->LastContextCount = turn->Context.Count;
  state->LastNewMessages = turn->NewMessages.Count;
  state->LastAssistantSeen = turn->Assistant && turn->Assistant->Kind == Role::Assistant;
  if (state->OverrideNextTurn) {
    out->HasModel = true;
    out->ModelInfo.Id = StrView_Dup(arena, SVL("override-model"));
    out->ModelInfo.Provider = StrView_Dup(arena, SVL("script"));
    out->ModelInfo.EffortMask = ModelEffort_Bit(ModelEffort::Medium);
    out->ModelInfo.DefaultEffort = ModelEffort::Medium;
    out->HasProfile = true;
    out->Profile.Effort = ModelEffort::Medium;
  }
  if (state->OverrideContext && state->PrepareCount == 1) {
    // A realistic trim: drop old history but keep the turn that just finished,
    // billed usage and all.
    out->HasContext = true;
    out->Context = Array_Make<Message>(arena, 2);
    Array_Push(&out->Context, Message_User(arena, SVL("override context")));
    if (state->KeepAssistantInOverride && turn->Assistant)
      Array_Push(&out->Context, *turn->Assistant);
  }
  return SessionResult_Ok();
}

SessionResult HookShouldStopAfterTurn(void* context, Arena* arena, const AgentTurnContext* turn,
                                      bool* outStop) {
  (void)arena;
  TurnHookState* state = (TurnHookState*)context;
  state->ShouldStopCount++;
  if (state->StopSeenCount < 8) state->StopSeenContext[state->StopSeenCount++] = turn->Context.Count;
  *outStop = state->StopAfterTurn != 0 && state->ShouldStopCount == state->StopAfterTurn;
  return SessionResult_Ok();
}

SessionResult HookSteeringMessages(void* context, Arena* arena, Array<Message>* out) {
  TurnHookState* state = (TurnHookState*)context;
  state->SteeringCount++;
  if (state->SteeringBudget == 0) return SessionResult_Ok();
  state->SteeringBudget--;
  Array_Push(out, Message_User(arena, SVL("hook steering")));
  return SessionResult_Ok();
}

SessionResult HookFollowUpMessages(void* context, Arena* arena, Array<Message>* out) {
  TurnHookState* state = (TurnHookState*)context;
  state->FollowUpCount++;
  if (state->FollowUpBudget == 0) return SessionResult_Ok();
  state->FollowUpBudget--;
  Array_Push(out, Message_User(arena, SVL("hook follow-up")));
  return SessionResult_Ok();
}

SessionResult FailPrepareNextTurn(void* context, Arena*, const AgentTurnContext*,
                                  AgentNextTurnUpdate*) {
  (*(u32*)context)++;
  return SessionResult_Error(SessionStatus::InvalidArgument,
                             SVL("prepare-next-turn hook failed"));
}

SessionResult FailSteeringMessages(void* context, Arena*, Array<Message>*) {
  (*(u32*)context)++;
  return SessionResult_Error(SessionStatus::InvalidArgument,
                             SVL("steering source failed"));
}

AgentHarnessConfig ScriptConfig(ScriptProvider* script, Tool* tools, u32 toolCount) {
  AgentHarnessConfig config = {};
  config.SystemPrompt = SVL("durable system");
  config.ModelInfo.Id = SVL("script-model");
  config.ModelInfo.Provider = SVL("script");
  config.Tools = tools;
  config.ToolCount = toolCount;
  config.ProviderImpl = ScriptProvider_Make(script);
  config.ToolExec = ToolExecution::Parallel;
  config.Retry.Enabled = true;
  config.Retry.MaxRetries = 3;
  config.Retry.BaseDelayMs = 1;
  config.Retry.MaxDelayMs = 4;
  return config;
}

u32 CountKind(const Array<SessionRecord>* records, SessionRecordKind kind) {
  u32 count = 0;
  for (u32 i = 0; i < records->Count; i++)
    if (records->Data[i].Kind == kind) count++;
  return count;
}

// True when every record after the session root is a plain Message -- the
// only kind an ordinary run/tool-batch/queue-flush ever appends in v2 (no
// operation/generation/tool-start/queue/cancel/deferred/recovery records).
bool AllNonRootAreMessages(const Array<SessionRecord>* records) {
  for (u32 i = 1; i < records->Count; i++)
    if (records->Data[i].Kind != SessionRecordKind::Message) return false;
  return true;
}

bool ContextTextEq(const Array<Message>* messages, u32 index, Role role, const char* text) {
  if (index >= messages->Count) return false;
  const Message* message = &messages->Data[index];
  return message->Kind == role && message->Content.Count > 0 &&
         StrView_EqCStr(message->Content.Data[0].Text, text);
}

// ---------------------------------------------------------------------------
// Watch-event classification: counts each transient/durable kind so tests can
// assert on meaningful totals (agent/turn/tool/retry/queue lifecycle, durable
// cursor contiguity) without hard-coding a single brittle grand total.
// ---------------------------------------------------------------------------

struct WatchTally {
  u32 Durable;
  u32 AgentStart, AgentEnd, TurnStart, TurnEnd;
  u32 ToolStart, ToolUpdate, ToolEnd;
  u32 RetryStart, RetryEnd;
  u32 QueueUpdate, CancelRequested;
  u32 StreamEvents;
  u64 LastDurableCursor;
  bool CursorMonotonic;
  RetryLayer LastRetryLayer;
  u32 LastRetryAttempt;
  u32 LastRetryMaxAttempts;
  bool RetryErrorMentionsOverload;
  AgentTurnOutcome AgentEndOutcome;
  bool AgentEndHasError;
  bool AgentEndErrorMentionsHook;
};

void TallyEvent(WatchTally* tally, const AgentHarnessEvent* event) {
  switch (event->Kind) {
    case AgentHarnessEventKind::DurableRecord:
      tally->Durable++;
      if (event->Cursor <= tally->LastDurableCursor) tally->CursorMonotonic = false;
      tally->LastDurableCursor = event->Cursor;
      break;
    case AgentHarnessEventKind::TransientAgentStart: tally->AgentStart++; break;
    case AgentHarnessEventKind::TransientAgentEnd:
      tally->AgentEnd++;
      tally->AgentEndOutcome = event->Turn.Outcome;
      tally->AgentEndHasError = event->Turn.Error.Length > 0;
      if (StrView_Contains(event->Turn.Error, "after-turn hook failed"))
        tally->AgentEndErrorMentionsHook = true;
      break;
    case AgentHarnessEventKind::TransientTurnStart: tally->TurnStart++; break;
    case AgentHarnessEventKind::TransientTurnEnd: tally->TurnEnd++; break;
    case AgentHarnessEventKind::TransientStream: tally->StreamEvents++; break;
    case AgentHarnessEventKind::TransientToolStart: tally->ToolStart++; break;
    case AgentHarnessEventKind::TransientToolUpdate: tally->ToolUpdate++; break;
    case AgentHarnessEventKind::TransientToolEnd: tally->ToolEnd++; break;
    case AgentHarnessEventKind::TransientRetryStart:
      tally->RetryStart++;
      tally->LastRetryLayer = event->Retry.Layer;
      tally->LastRetryAttempt = event->Retry.Attempt;
      tally->LastRetryMaxAttempts = event->Retry.MaxAttempts;
      if (StrView_Contains(event->Retry.Error, "overloaded")) tally->RetryErrorMentionsOverload = true;
      break;
    case AgentHarnessEventKind::TransientRetryEnd: tally->RetryEnd++; break;
    case AgentHarnessEventKind::TransientQueueUpdate: tally->QueueUpdate++; break;
    case AgentHarnessEventKind::TransientCancelRequested: tally->CancelRequested++; break;
  }
}

WatchTally DrainUntilAgentEnd(Arena* arena, AgentHarnessWatch* watch) {
  WatchTally tally = {};
  tally.CursorMonotonic = true;
  for (;;) {
    AgentHarnessEvent event = {};
    SessionResult next = AgentHarness_WatchNext(arena, watch, &event);
    if (!SessionResult_IsOk(next)) break;
    TallyEvent(&tally, &event);
    if (event.Kind == AgentHarnessEventKind::TransientAgentEnd) break;
  }
  return tally;
}

struct CancelProviderState {
  volatile i32 Started;
};

bool CancelProviderStream(void* context, Arena* arena, const Model* model, const LlmContext* llm,
                          const StreamOptions* options, StreamSink sink, Message* out) {
  (void)arena;
  (void)model;
  (void)llm;
  (void)sink;
  CancelProviderState* state = (CancelProviderState*)context;
  state->Started = 1;
  while (!options->Abort || !*options->Abort) Sleep_Ms(1);
  *out = Message{};
  out->Kind = Role::Assistant;
  out->Stop = StopReason::Aborted;
  out->Timestamp = NowMillis();
  return false;
}

struct AbortContinueProviderState {
  volatile i32 Started;
  u32 Calls;
  bool FollowUpContextValid;
};

bool AbortContinueProviderStream(void* context, Arena* arena, const Model* model,
                                 const LlmContext* llm, const StreamOptions* options,
                                 StreamSink sink, Message* out) {
  (void)sink;
  AbortContinueProviderState* state = (AbortContinueProviderState*)context;
  state->Calls++;
  if (state->Calls == 1) {
    state->Started = 1;
    while (!options->Abort || !*options->Abort) Sleep_Ms(1);
    *out = Message_AssistantEmpty(arena);
    out->Provider = model->Provider;
    out->Model = model->Id;
    out->Stop = StopReason::Aborted;
    out->ErrorMessage = SVL("aborted");
    return false;
  }

  state->FollowUpContextValid =
      llm->MessageCount == 2 && llm->Messages[0].Kind == Role::User &&
      llm->Messages[0].Content.Count == 1 &&
      StrView_EqCStr(llm->Messages[0].Content.Data[0].Text, "cancel me") &&
      llm->Messages[1].Kind == Role::User && llm->Messages[1].Content.Count == 1 &&
      StrView_EqCStr(llm->Messages[1].Content.Data[0].Text, "after cancel");
  *out = Message_AssistantEmpty(arena);
  out->Provider = model->Provider;
  out->Model = model->Id;
  out->Stop = StopReason::Stop;
  Array_Push(&out->Content, Content_Text(SVL("continued")));
  return true;
}

struct UsageRetryProviderState {
  u32 Calls;
};

bool UsageRetryProviderStream(void* context, Arena* arena, const Model* model, const LlmContext* llm,
                              const StreamOptions* options, StreamSink sink, Message* out) {
  (void)llm;
  (void)options;
  (void)sink;
  UsageRetryProviderState* state = (UsageRetryProviderState*)context;
  *out = Message_AssistantEmpty(arena);
  out->Provider = model->Provider;
  out->Model = model->Id;
  if (state->Calls++ == 0) {
    out->Stop = StopReason::Error;
    out->ErrorMessage = SVL("provider overloaded; please retry");
    out->Use = Usage{3, 2, 1, 0, 0, 9};
    return false;
  }
  out->Stop = StopReason::Stop;
  out->Use = Usage{5, 4, 2, 1, 3, 0};
  Array_Push(&out->Content, Content_Text(SVL("completed")));
  return true;
}

struct RetryPolicyCaptureState {
  const HttpRetryPolicy* Expected;
  bool WasCopied;
  bool Enabled;
  bool HasEvent;
  u32 MaxRetries;
};

bool RetryPolicyCaptureStream(void* context, Arena* arena, const Model* model, const LlmContext* llm,
                              const StreamOptions* options, StreamSink sink, Message* out) {
  (void)model;
  (void)llm;
  (void)sink;
  RetryPolicyCaptureState* state = (RetryPolicyCaptureState*)context;
  state->WasCopied = options->Retry != state->Expected;
  state->Enabled = options->Retry->Enabled;
  state->HasEvent = options->Retry->OnEvent != nullptr;
  state->MaxRetries = options->Retry->MaxRetries;
  HttpRetryEvent start = {};
  start.Kind = HttpRetryEventKind::Start;
  start.Attempt = 1;
  start.MaxRetries = options->Retry->MaxRetries;
  start.DelayMs = 1;
  start.Status = 429;
  start.Error = SVL("provider overloaded");
  options->Retry->OnEvent(options->Retry->EventUser, &start);
  HttpRetryEvent end = start;
  end.Kind = HttpRetryEventKind::End;
  options->Retry->OnEvent(options->Retry->EventUser, &end);
  *out = Message_AssistantEmpty(arena);
  out->Stop = StopReason::Stop;
  Array_Push(&out->Content, Content_Text(SVL("ok")));
  return true;
}

// Fails every attempt outright (no retry configured), so the run stops on
// the very first turn with a durable error message -- used to check that
// TransientAgentEnd/TransientTurnEnd carry AgentTurnOutcome::Failed and the
// provider's error text, not just the success path.
bool ImmediateErrorProviderStream(void* context, Arena* arena, const Model* model,
                                  const LlmContext* llm, const StreamOptions* options,
                                  StreamSink sink, Message* out) {
  (void)context;
  (void)llm;
  (void)options;
  (void)sink;
  *out = Message_AssistantEmpty(arena);
  out->Provider = model->Provider;
  out->Model = model->Id;
  out->Stop = StopReason::Error;
  out->ErrorMessage = SVL("immediate failure for lifecycle test");
  return false;
}

struct OrphanCaptureState {
  Message Captured[8];
  u32 CapturedCount;
};

bool OrphanCaptureStream(void* context, Arena* arena, const Model* model, const LlmContext* llm,
                         const StreamOptions* options, StreamSink sink, Message* out) {
  (void)model;
  (void)options;
  (void)sink;
  OrphanCaptureState* state = (OrphanCaptureState*)context;
  state->CapturedCount = llm->MessageCount < 8 ? llm->MessageCount : 8;
  for (u32 i = 0; i < state->CapturedCount; i++) state->Captured[i] = llm->Messages[i];
  Message m = Message_AssistantEmpty(arena);
  m.Stop = StopReason::Stop;
  Array_Push(&m.Content, Content_Text(SVL("continued after restart")));
  *out = m;
  return true;
}

struct CountingToolCallProviderState {
  u32 Calls;
};

bool CountingToolCallProviderStream(void* context, Arena* arena, const Model* model,
                                    const LlmContext* llm, const StreamOptions* options,
                                    StreamSink sink, Message* out) {
  (void)llm;
  (void)options;
  (void)sink;
  CountingToolCallProviderState* state = (CountingToolCallProviderState*)context;
  state->Calls++;
  Message m = Message_AssistantEmpty(arena);
  m.Provider = model->Provider;
  m.Model = model->Id;
  m.Stop = StopReason::ToolUse;
  Array_Push(&m.Content, Content_ToolCall(SVL("call-x"), SVL("delay"), SVL("{\"label\":\"x\",\"delay\":30}")));
  Array_Push(&m.Content, Content_ToolCall(SVL("call-y"), SVL("delay"), SVL("{\"label\":\"y\",\"delay\":30}")));
  *out = m;
  return true;
}

struct HarnessRunThread {
  AgentHarness* Harness;
  StrView Prompt;
  SessionResult Result;
};

void RunHarnessThread(void* context) {
  HarnessRunThread* run = (HarnessRunThread*)context;
  run->Result = AgentHarness_Run(run->Harness, run->Prompt);
}

struct WatchWaitThread {
  AgentHarnessWatch* Watch;
  volatile i32 Started;
  SessionResult Result;
};

void WaitForWatchThread(void* context) {
  WatchWaitThread* wait = (WatchWaitThread*)context;
  Arena arena = ArenaCreate();
  AgentHarnessEvent event = {};
  wait->Started = 1;
  wait->Result = AgentHarness_WatchNext(&arena, wait->Watch, &event);
  ArenaRelease(&arena);
}

}  // namespace

static void TestHarnessReplayAndIncremental() {
  // Provider replay excludes incomplete assistant turns even when they
  // contain partial output, while preserving the surrounding user messages
  // (session/replay.h; see test_session.cpp TestReplay for direct coverage).
  {
    Arena arena = ArenaCreate();
    Message messages[5];
    messages[0] = Message_User(&arena, SVL("first"));
    messages[1] = Message_AssistantEmpty(&arena);
    messages[1].Stop = StopReason::Aborted;
    Array_Push(&messages[1].Content, Content_Text(SVL("partial abort")));
    messages[2] = Message_User(&arena, SVL("second"));
    messages[3] = Message_AssistantEmpty(&arena);
    messages[3].Stop = StopReason::Error;
    Array_Push(&messages[3].Content, Content_Text(SVL("partial error")));
    messages[4] = Message_User(&arena, SVL("third"));
    AgentProviderRequest request = {};
    AgentProvider_BuildRequest(&arena, SVL("system"), messages, 5, nullptr, 0, &request);
    const Message* replay = request.Context.Messages;
    CHECK_ALL(request.Context.MessageCount == 3, replay[0].Kind == Role::User,
              StrView_EqCStr(replay[0].Content.Data[0].Text, "first"), replay[1].Kind == Role::User,
              StrView_EqCStr(replay[1].Content.Data[0].Text, "second"), replay[2].Kind == Role::User,
              StrView_EqCStr(replay[2].Content.Data[0].Text, "third"));
    ArenaRelease(&arena);
  }

  // Public snapshots own their records and indexes through the caller arena,
  // so they remain usable after the harness releases its internal state.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    ScriptStep step = {};
    step.Text = SVL("snapshot survives close");
    step.Stop = StopReason::Stop;
    ScriptProvider script = {&step, 1, 0};
    AgentHarnessConfig config = ScriptConfig(&script, nullptr, 0);
    AgentHarness harness = {};
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("snapshot me"))));
    SessionReduction snapshot = {};
    CHECK(SessionResult_IsOk(
        AgentHarness_Snapshot(&arena, &harness, &snapshot)));
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));

    Array<Message> context = {};
    CHECK(SessionResult_IsOk(SessionReducer_Context(
        &arena, &snapshot, snapshot.HeadId, &context)));
    CHECK_ALL(context.Count == 2,
              ContextTextEq(&context, 0, Role::User, "snapshot me"),
              ContextTextEq(&context, 1, Role::Assistant,
                            "snapshot survives close"));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Core deterministic proof: AgentHarness_Init performs exactly one ReadAll;
  // every later operation (run, tool batch, compaction, navigation, model/
  // name/label changes, snapshot, watch, fork) folds incrementally and never
  // rereads the log. Plain-run persistence is message-only.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));

    u32 executions = 0;
    Tool echo = {};
    echo.Name = SVL("echo");
    echo.Description = SVL("echo arguments");
    echo.ParamsJsonSchema = SVL("{\"type\":\"object\"}");
    echo.Ctx = &executions;
    echo.Execute = EchoExec;
    ScriptToolCall calls[1] = {ScriptToolCall{SVL("echo"), SVL("call-1"), SVL("{\"x\":1}")}};
    ScriptStep steps[3] = {};
    steps[0].Tools = calls;
    steps[0].ToolCount = 1;
    steps[1].Text = SVL("done");
    steps[1].Stop = StopReason::Stop;
    steps[2].Text = SVL("compact summary");
    steps[2].Stop = StopReason::Stop;
    ScriptProvider script = {steps, 3, 0};

    AgentHarness harness = {};
    AgentHarnessConfig config = ScriptConfig(&script, &echo, 1);
    u64 baseline = memory.ReadAllCalls;
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    CHECK(memory.ReadAllCalls == baseline + 1);

    CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("hello"))));
    CHECK(memory.ReadAllCalls == baseline + 1);

    Array<SessionRecord> afterRun = {};
    CHECK(SessionResult_IsOk(SessionLog_ReadAll(&harness.Log, &arena, &afterRun)));
    u64 afterRunCalls = memory.ReadAllCalls;
    CHECK(afterRunCalls == baseline + 2);  // this explicit browsing ReadAll is expected to count
    CHECK_ALL(executions == 1, afterRun.Count == 5, AllNonRootAreMessages(&afterRun));

    SessionReduction snap = {};
    CHECK(SessionResult_IsOk(AgentHarness_Snapshot(&arena, &harness, &snap)));
    CHECK(memory.ReadAllCalls == afterRunCalls);
    Array<Message> context = {};
    CHECK(SessionResult_IsOk(SessionReducer_Context(&arena, &snap, snap.HeadId, &context)));
    CHECK_ALL(context.Count == 4, ContextTextEq(&context, 0, Role::User, "hello"),
              context.Data[1].Kind == Role::Assistant,
              ContextTextEq(&context, 2, Role::ToolResult, "echo:{\"x\":1}"),
              ContextTextEq(&context, 3, Role::Assistant, "done"));

    CHECK(SessionResult_IsOk(AgentHarness_Compact(&harness)));
    CHECK(memory.ReadAllCalls == afterRunCalls);
    SessionReduction compacted = {};
    CHECK(SessionResult_IsOk(AgentHarness_Snapshot(&arena, &harness, &compacted)));
    CHECK(memory.ReadAllCalls == afterRunCalls);
    DurableId compactedHead = compacted.HeadId;

    CHECK(SessionResult_IsOk(AgentHarness_LabelBranch(&harness, SVL("main"))));
    CHECK(SessionResult_IsOk(AgentHarness_SetName(&harness, SVL("deterministic"))));
    CHECK(memory.ReadAllCalls == afterRunCalls);

    Model model = {};
    model.Provider = SVL("script-2");
    model.Id = SVL("model-2");
    ScriptProvider second = {};
    Provider secondProvider = ScriptProvider_Make(&second);
    CHECK(SessionResult_IsOk(
        AgentHarness_SetModel(&harness, model, secondProvider, ModelRequestProfile{})));
    CHECK(memory.ReadAllCalls == afterRunCalls);

    CHECK(SessionResult_IsOk(AgentHarness_Navigate(&harness, compactedHead)));
    CHECK(memory.ReadAllCalls == afterRunCalls);

    AgentHarnessSnapshot full = {};
    CHECK(SessionResult_IsOk(AgentHarness_SnapshotFull(&arena, &harness, &full)));
    CHECK(memory.ReadAllCalls == afterRunCalls);

    AgentHarnessSnapshot watchSnapshot = {};
    AgentHarnessWatch* watchPtr = ArenaPushStruct(&arena, AgentHarnessWatch);
    *watchPtr = AgentHarnessWatch{};
    AgentHarnessWatch& watch = *watchPtr;
    CHECK(SessionResult_IsOk(AgentHarness_Watch(&arena, &harness, 16, &watchSnapshot, &watch)));
    CHECK(memory.ReadAllCalls == afterRunCalls);
    CHECK(SessionResult_IsOk(AgentHarness_WatchClose(&watch)));

    SessionLog forkLog = {};
    SessionInfo forkInfo = {};
    CHECK(SessionResult_IsOk(AgentHarness_Fork(&arena, &harness, &repository, DurableId{}, DurableId{},
                                              SVL("forked"), &forkLog, &forkInfo)));
    CHECK(memory.ReadAllCalls == afterRunCalls);
    SessionLog_Close(&forkLog);

    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Provider usage lives on the assistant message itself; a retried attempt's
  // usage never reaches storage since v2 has no durable per-attempt
  // generation record -- only the final, successful attempt's message does,
  // and exactly one assistant message is ever persisted for the turn.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    UsageRetryProviderState providerState = {};
    AgentHarnessConfig config = {};
    config.SystemPrompt = SVL("usage");
    config.ModelInfo.Id = SVL("usage-model");
    config.ModelInfo.Provider = SVL("usage-provider");
    config.ProviderImpl.Ctx = &providerState;
    config.ProviderImpl.Stream = UsageRetryProviderStream;
    config.Retry.Enabled = true;
    config.Retry.MaxRetries = 1;
    config.Retry.BaseDelayMs = 1;
    config.Retry.MaxDelayMs = 1;
    AgentHarness harness = {};
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("go"))));
    Array<SessionRecord> records = {};
    CHECK(SessionResult_IsOk(SessionLog_ReadAll(&harness.Log, &arena, &records)));
    SessionMetadataBuildOptions build = {};
    SessionInfo info = {};
    CHECK(SessionResult_IsOk(
        SessionMetadata_Build(&arena, records.Data, records.Count, &build, &info)));
    CHECK_ALL(providerState.Calls == 2, info.AssistantMessageCount == 1, info.TotalTokens == 12,
              info.InputTokens == 5, info.OutputTokens == 4, info.CacheReadTokens == 2,
              info.CacheWriteTokens == 1, info.ReasoningTokens == 3);
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // The assistant-turn retry layer (Config.Retry) emits a matched
  // retry_start/retry_end pair around its backoff, tagged AssistantTurn.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    UsageRetryProviderState providerState = {};
    AgentHarnessConfig config = {};
    config.SystemPrompt = SVL("retry events");
    config.ModelInfo.Id = SVL("retry-model");
    config.ModelInfo.Provider = SVL("retry-provider");
    config.ProviderImpl.Ctx = &providerState;
    config.ProviderImpl.Stream = UsageRetryProviderStream;
    config.Retry.Enabled = true;
    config.Retry.MaxRetries = 1;
    config.Retry.BaseDelayMs = 1;
    config.Retry.MaxDelayMs = 2;
    AgentHarness harness = {};
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    AgentHarnessSnapshot initial = {};
    AgentHarnessWatch* watchPtr = ArenaPushStruct(&arena, AgentHarnessWatch);
    *watchPtr = AgentHarnessWatch{};
    AgentHarnessWatch& watch = *watchPtr;
    CHECK(SessionResult_IsOk(AgentHarness_Watch(&arena, &harness, 32, &initial, &watch)));
    CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("go"))));

    WatchTally tally = DrainUntilAgentEnd(&arena, &watch);
    CHECK_ALL(tally.RetryStart == 1, tally.RetryEnd == 1, tally.AgentEnd == 1,
              tally.LastRetryLayer == RetryLayer::AssistantTurn, tally.LastRetryAttempt == 1,
              tally.LastRetryMaxAttempts == 1, tally.RetryErrorMentionsOverload);
    CHECK(SessionResult_IsOk(AgentHarness_WatchClose(&watch)));
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Config.Stream.Retry is copied per provider attempt so the harness can
  // attach ProviderRequest lifecycle events without mutating caller-owned
  // policy state.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    HttpRetryPolicy policy = HttpRetryPolicy_Default();
    policy.Enabled = true;
    RetryPolicyCaptureState providerState = {};
    providerState.Expected = &policy;
    AgentHarnessConfig config = {};
    config.SystemPrompt = SVL("provider request retry");
    config.ModelInfo.Id = SVL("capture-model");
    config.ModelInfo.Provider = SVL("capture-provider");
    config.ProviderImpl.Ctx = &providerState;
    config.ProviderImpl.Stream = RetryPolicyCaptureStream;
    config.Stream.Retry = &policy;
    AgentHarness harness = {};
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    AgentHarnessSnapshot initial = {};
    AgentHarnessWatch* watchPtr = ArenaPushStruct(&arena, AgentHarnessWatch);
    *watchPtr = AgentHarnessWatch{};
    AgentHarnessWatch& watch = *watchPtr;
    CHECK(SessionResult_IsOk(AgentHarness_Watch(&arena, &harness, 32, &initial, &watch)));
    CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("go"))));
    WatchTally tally = DrainUntilAgentEnd(&arena, &watch);
    CHECK_ALL(providerState.WasCopied, providerState.Enabled,
              providerState.HasEvent, providerState.MaxRetries == policy.MaxRetries,
              tally.RetryStart == 1, tally.RetryEnd == 1,
              tally.LastRetryLayer == RetryLayer::ProviderRequest,
              tally.LastRetryAttempt == 1,
              tally.LastRetryMaxAttempts == policy.MaxRetries,
              tally.RetryErrorMentionsOverload);
    CHECK(SessionResult_IsOk(AgentHarness_WatchClose(&watch)));
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

}

static void TestHarnessLifecycle() {
  // Every transient lifecycle event kind exposes a complete, self-contained
  // AgentHarnessEvent -- this is the v2 replacement for deriving
  // AgentStart/TurnStart/ToolStart/AgentEnd from the now-removed durable
  // OperationStarted/ToolStarted/OperationFinished records: app/TUI code can
  // translate straight from these fields (Turn, ToolCallId/ToolName/
  // ToolArgsJson, Outcome/Error) without ever touching the tree.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));

    u32 executions = 0;
    Tool echo = {};
    echo.Name = SVL("echo");
    echo.Description = SVL("echo arguments");
    echo.ParamsJsonSchema = SVL("{\"type\":\"object\"}");
    echo.Ctx = &executions;
    echo.Execute = EchoExec;
    ScriptToolCall calls[1] = {ScriptToolCall{SVL("echo"), SVL("call-lifecycle"), SVL("{\"y\":2}")}};
    ScriptStep steps[2] = {};
    steps[0].Tools = calls;
    steps[0].ToolCount = 1;
    steps[1].Text = SVL("lifecycle done");
    steps[1].Stop = StopReason::Stop;
    ScriptProvider script = {steps, 2, 0};

    AgentHarness harness = {};
    AgentHarnessConfig config = ScriptConfig(&script, &echo, 1);
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    AgentHarnessSnapshot initial = {};
    AgentHarnessWatch* watchPtr = ArenaPushStruct(&arena, AgentHarnessWatch);
    *watchPtr = AgentHarnessWatch{};
    AgentHarnessWatch& watch = *watchPtr;
    CHECK(SessionResult_IsOk(AgentHarness_Watch(&arena, &harness, 64, &initial, &watch)));
    CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("lifecycle prompt"))));

    bool sawAgentStart = false, sawTurnStart = false, sawToolStart = false, sawAgentEnd = false;
    AgentTurnKind agentStartTurn = {}, turnStartTurn = {}, agentEndTurn = {};
    AgentTurnOutcome agentEndOutcome = {};
    StrView toolStartCallId = {}, toolStartName = {}, toolStartArgs = {};
    u32 turnStartCount = 0, turnEndCount = 0;
    bool finished = false;
    while (!finished) {
      AgentHarnessEvent event = {};
      CHECK(SessionResult_IsOk(AgentHarness_WatchNext(&arena, &watch, &event)));
      switch (event.Kind) {
        case AgentHarnessEventKind::TransientAgentStart:
          sawAgentStart = true;
          agentStartTurn = event.Turn.Kind;
          break;
        case AgentHarnessEventKind::TransientTurnStart:
          sawTurnStart = true;
          turnStartTurn = event.Turn.Kind;
          turnStartCount++;
          break;
        case AgentHarnessEventKind::TransientTurnEnd:
          turnEndCount++;
          break;
        case AgentHarnessEventKind::TransientToolStart:
          sawToolStart = true;
          toolStartCallId = StrView_Dup(&arena, event.Tool.CallId);
          toolStartName = StrView_Dup(&arena, event.Tool.Name);
          toolStartArgs = StrView_Dup(&arena, event.Tool.ArgumentsJson);
          break;
        case AgentHarnessEventKind::TransientAgentEnd:
          sawAgentEnd = true;
          agentEndTurn = event.Turn.Kind;
          agentEndOutcome = event.Turn.Outcome;
          finished = true;
          break;
        default:
          break;
      }
    }
    CHECK_ALL(sawAgentStart, agentStartTurn == AgentTurnKind::Run, sawTurnStart,
              turnStartTurn == AgentTurnKind::Run, turnStartCount == 2, turnEndCount == 2,
              sawToolStart, StrView_EqCStr(toolStartCallId, "call-lifecycle"),
              StrView_EqCStr(toolStartName, "echo"), StrView_Contains(toolStartArgs, "\"y\":2"),
              sawAgentEnd, agentEndTurn == AgentTurnKind::Run,
              agentEndOutcome == AgentTurnOutcome::Succeeded);

    CHECK(SessionResult_IsOk(AgentHarness_WatchClose(&watch)));
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // A turn that fails outright (no retry configured) still publishes
  // TransientTurnEnd/TransientAgentEnd tagged AgentTurnOutcome::Failed with
  // the provider's error text -- not just AgentTurnOutcome::Succeeded.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    AgentHarnessConfig config = {};
    config.SystemPrompt = SVL("lifecycle failure");
    config.ModelInfo.Id = SVL("fail-model");
    config.ModelInfo.Provider = SVL("test");
    config.ProviderImpl.Stream = ImmediateErrorProviderStream;
    AgentHarness harness = {};
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    AgentHarnessSnapshot initial = {};
    AgentHarnessWatch* watchPtr = ArenaPushStruct(&arena, AgentHarnessWatch);
    *watchPtr = AgentHarnessWatch{};
    AgentHarnessWatch& watch = *watchPtr;
    CHECK(SessionResult_IsOk(AgentHarness_Watch(&arena, &harness, 32, &initial, &watch)));
    CHECK(AgentHarness_Run(&harness, SVL("will fail")).Status == SessionStatus::ProviderError);

    bool sawFailedTurnEnd = false, sawFailedAgentEnd = false;
    StrView turnEndError = {}, agentEndError = {};
    bool finished = false;
    while (!finished) {
      AgentHarnessEvent event = {};
      CHECK(SessionResult_IsOk(AgentHarness_WatchNext(&arena, &watch, &event)));
      if (event.Kind == AgentHarnessEventKind::TransientTurnEnd &&
          event.Turn.Outcome == AgentTurnOutcome::Failed) {
        sawFailedTurnEnd = true;
        turnEndError = StrView_Dup(&arena, event.Turn.Error);
      } else if (event.Kind == AgentHarnessEventKind::TransientAgentEnd) {
        sawFailedAgentEnd = event.Turn.Outcome == AgentTurnOutcome::Failed;
        agentEndError = StrView_Dup(&arena, event.Turn.Error);
        finished = true;
      }
    }
    CHECK_ALL(sawFailedTurnEnd, StrView_Contains(turnEndError, "immediate failure"),
              sawFailedAgentEnd, StrView_Contains(agentEndError, "immediate failure"));

    CHECK(SessionResult_IsOk(AgentHarness_WatchClose(&watch)));
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Compaction turns are tagged AgentTurnKind::Compaction throughout their
  // lifecycle events (distinct from AgentTurnKind::Run), and queuing while
  // idle publishes a TransientQueueUpdate with correctly incrementing counts
  // even with no run in progress.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    ScriptStep step = {};
    step.Text = SVL("compaction lifecycle summary");
    step.Stop = StopReason::Stop;
    ScriptProvider script = {&step, 1, 0};
    AgentHarness harness = {};
    AgentHarnessConfig config = ScriptConfig(&script, nullptr, 0);
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    AgentHarnessSnapshot initial = {};
    AgentHarnessWatch* watchPtr = ArenaPushStruct(&arena, AgentHarnessWatch);
    *watchPtr = AgentHarnessWatch{};
    AgentHarnessWatch& watch = *watchPtr;
    CHECK(SessionResult_IsOk(AgentHarness_Watch(&arena, &harness, 32, &initial, &watch)));

    CHECK(SessionResult_IsOk(AgentHarness_QueueSteering(&harness, SVL("idle steer"))));
    CHECK(SessionResult_IsOk(AgentHarness_QueueFollowUp(&harness, SVL("idle follow-up"))));
    CHECK(SessionResult_IsOk(AgentHarness_QueueNextTurn(&harness, SVL("idle next-turn"))));
    u32 lastSteer = 0, lastFollowUp = 0, lastNextTurn = 0;
    u32 queueUpdateCount = 0;
    for (u32 i = 0; i < 3; i++) {
      AgentHarnessEvent event = {};
      CHECK(SessionResult_IsOk(AgentHarness_WatchNext(&arena, &watch, &event)));
      CHECK(event.Kind == AgentHarnessEventKind::TransientQueueUpdate);
      queueUpdateCount++;
      lastSteer = event.Queue.SteerCount;
      lastFollowUp = event.Queue.FollowUpCount;
      lastNextTurn = event.Queue.NextTurnCount;
    }
    CHECK_ALL(queueUpdateCount == 3, lastSteer == 1, lastFollowUp == 1, lastNextTurn == 1);

    CHECK(SessionResult_IsOk(AgentHarness_Compact(&harness)));
    bool sawCompactionAgentStart = false, sawCompactionTurnStart = false;
    bool sawCompactionAgentEnd = false;
    bool finished = false;
    while (!finished) {
      AgentHarnessEvent event = {};
      CHECK(SessionResult_IsOk(AgentHarness_WatchNext(&arena, &watch, &event)));
      if (event.Kind == AgentHarnessEventKind::TransientAgentStart)
        sawCompactionAgentStart = event.Turn.Kind == AgentTurnKind::Compaction;
      else if (event.Kind == AgentHarnessEventKind::TransientTurnStart)
        sawCompactionTurnStart = event.Turn.Kind == AgentTurnKind::Compaction;
      else if (event.Kind == AgentHarnessEventKind::TransientAgentEnd) {
        sawCompactionAgentEnd =
            event.Turn.Kind == AgentTurnKind::Compaction && event.Turn.Outcome == AgentTurnOutcome::Succeeded;
        finished = true;
      }
    }
    CHECK_ALL(sawCompactionAgentStart, sawCompactionTurnStart, sawCompactionAgentEnd);

    CHECK(SessionResult_IsOk(AgentHarness_WatchClose(&watch)));
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // A storage failure while appending the compaction record is reflected in
  // both the returned result and the final lifecycle event.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    ScriptStep step = {};
    step.Text = SVL("summary that cannot be stored");
    step.Stop = StopReason::Stop;
    ScriptProvider script = {&step, 1, 0};
    AgentHarnessConfig config = ScriptConfig(&script, nullptr, 0);
    AgentHarness harness = {};
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    AgentHarnessSnapshot initial = {};
    AgentHarnessWatch* watchPtr = ArenaPushStruct(&arena, AgentHarnessWatch);
    *watchPtr = AgentHarnessWatch{};
    AgentHarnessWatch& watch = *watchPtr;
    CHECK(SessionResult_IsOk(
        AgentHarness_Watch(&arena, &harness, 16, &initial, &watch)));
    MemoryRepository_FailAfterAppends(&memory, 0);
    CHECK(AgentHarness_Compact(&harness).Status == SessionStatus::IoError);
    WatchTally tally = DrainUntilAgentEnd(&arena, &watch);
    CHECK_ALL(tally.AgentEnd == 1,
              tally.AgentEndOutcome == AgentTurnOutcome::Failed,
              tally.AgentEndHasError);
    CHECK(SessionResult_IsOk(AgentHarness_WatchClose(&watch)));
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

}

static void TestHarnessWatchers() {
  // Watch atomically returns a snapshot cursor and then delivers every
  // durable transition plus transient agent/turn/provider lifecycle events in
  // publication order; message-only persistence means far fewer durable
  // events than transient ones.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    ScriptStep step = {};
    step.Text = SVL("watched response");
    step.Stop = StopReason::Stop;
    ScriptProvider script = {&step, 1, 0};
    AgentHarness harness = {};
    AgentHarnessConfig config = ScriptConfig(&script, nullptr, 0);
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));

    AgentHarnessSnapshot initial = {};
    AgentHarnessWatch* watchPtr = ArenaPushStruct(&arena, AgentHarnessWatch);
    *watchPtr = AgentHarnessWatch{};
    AgentHarnessWatch& watch = *watchPtr;
    CHECK(SessionResult_IsOk(AgentHarness_Watch(&arena, &harness, 16, &initial, &watch)));
    CHECK_ALL(initial.Cursor == 1, initial.Transcript.Count == 0, !initial.Running, !initial.Faulted);
    CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("watched prompt"))));

    u32 durableCount = 0;
    u32 transientCount = 0;
    bool sawDelta = false;
    u64 durableCursor = initial.Cursor;
    bool finished = false;
    while (!finished) {
      AgentHarnessEvent event = {};
      CHECK(SessionResult_IsOk(AgentHarness_WatchNext(&arena, &watch, &event)));
      if (event.Kind == AgentHarnessEventKind::DurableRecord) {
        durableCount++;
        durableCursor++;
        CHECK_ALL(event.Cursor == durableCursor, event.Record->Seq == event.Cursor);
      } else {
        transientCount++;
        if (event.Kind == AgentHarnessEventKind::TransientStream &&
            event.Stream.Kind == StreamEventKind::TextDelta) {
          sawDelta = true;
          CHECK(StrView_EqCStr(event.Stream.Delta, "watched response"));
        }
        if (event.Kind == AgentHarnessEventKind::TransientAgentEnd) finished = true;
      }
    }
    CHECK_ALL(durableCount == 2, transientCount == 9, sawDelta, durableCursor == initial.Cursor + 2);

    AgentHarnessSnapshot complete = {};
    CHECK(SessionResult_IsOk(AgentHarness_SnapshotFull(&arena, &harness, &complete)));
    CHECK_ALL(complete.Cursor == initial.Cursor + 2, complete.Transcript.Count == 2,
              ContextTextEq(&complete.Transcript, 0, Role::User, "watched prompt"),
              ContextTextEq(&complete.Transcript, 1, Role::Assistant, "watched response"));
    CHECK(SessionResult_IsOk(AgentHarness_WatchClose(&watch)));
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Slow watchers fail explicitly and require a new snapshot instead of
  // silently dropping events.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    ScriptStep step = {};
    step.Text = SVL("overflow");
    step.Stop = StopReason::Stop;
    ScriptProvider script = {&step, 1, 0};
    AgentHarness harness = {};
    AgentHarnessConfig config = ScriptConfig(&script, nullptr, 0);
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    AgentHarnessSnapshot snapshot = {};
    AgentHarnessWatch* watchPtr = ArenaPushStruct(&arena, AgentHarnessWatch);
    *watchPtr = AgentHarnessWatch{};
    AgentHarnessWatch& watch = *watchPtr;
    CHECK(SessionResult_IsOk(AgentHarness_Watch(&arena, &harness, 2, &snapshot, &watch)));
    CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("overflow prompt"))));
    AgentHarnessEvent event = {};
    CHECK(AgentHarness_WatchNext(&arena, &watch, &event).Status == SessionStatus::WatcherLagged);
    CHECK(SessionResult_IsOk(AgentHarness_WatchClose(&watch)));
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Concurrent control-plane calls (steering, cancel) while a provider call
  // is blocked are memory-only -- they never append a durable record -- yet
  // the watcher still sees the run's own durable records in strict,
  // gap-free cursor order.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    CancelProviderState providerState = {};
    AgentHarnessConfig config = {};
    config.SystemPrompt = SVL("watch concurrency");
    config.ModelInfo.Id = SVL("cancel-model");
    config.ModelInfo.Provider = SVL("test");
    config.ProviderImpl.Ctx = &providerState;
    config.ProviderImpl.Stream = CancelProviderStream;
    config.ToolExec = ToolExecution::Parallel;
    AgentHarness harness = {};
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    AgentHarnessSnapshot snapshot = {};
    AgentHarnessWatch* watchPtr = ArenaPushStruct(&arena, AgentHarnessWatch);
    *watchPtr = AgentHarnessWatch{};
    AgentHarnessWatch& watch = *watchPtr;
    CHECK(SessionResult_IsOk(AgentHarness_Watch(&arena, &harness, 32, &snapshot, &watch)));

    HarnessRunThread run = {};
    run.Harness = &harness;
    run.Prompt = SVL("cancel me");
    Thread thread = {};
    CHECK(Thread_Start(&thread, RunHarnessThread, &run));
    u32 waited = 0;
    while (!providerState.Started && waited < 2000) {
      Sleep_Ms(1);
      waited++;
    }
    CHECK(providerState.Started != 0);
    CHECK(SessionResult_IsOk(AgentHarness_QueueSteering(&harness, SVL("queued concurrently"))));
    CHECK(SessionResult_IsOk(AgentHarness_RequestCancel(&harness)));
    Thread_Join(&thread);
    CHECK(run.Result.Status == SessionStatus::Cancelled);

    WatchTally tally = DrainUntilAgentEnd(&arena, &watch);
    CHECK_ALL(tally.CursorMonotonic, tally.Durable == 2, tally.AgentStart == 1, tally.AgentEnd == 1,
              tally.TurnStart == 1, tally.TurnEnd == 1, tally.QueueUpdate >= 1,
              tally.CancelRequested >= 1);

    Array<SessionRecord> records = {};
    CHECK(SessionResult_IsOk(SessionLog_ReadAll(&harness.Log, &arena, &records)));
    CHECK(tally.LastDurableCursor == records.Data[records.Count - 1].Seq);
    CHECK(SessionResult_IsOk(AgentHarness_WatchClose(&watch)));
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Closing a watcher wakes a blocked consumer before the watcher resources
  // are destroyed.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    ScriptProvider script = {};
    AgentHarness harness = {};
    AgentHarnessConfig config = ScriptConfig(&script, nullptr, 0);
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    AgentHarnessSnapshot snapshot = {};
    AgentHarnessWatch* watchPtr = ArenaPushStruct(&arena, AgentHarnessWatch);
    *watchPtr = AgentHarnessWatch{};
    AgentHarnessWatch& watch = *watchPtr;
    CHECK(SessionResult_IsOk(AgentHarness_Watch(&arena, &harness, 4, &snapshot, &watch)));
    WatchWaitThread wait = {};
    wait.Watch = &watch;
    Thread thread = {};
    CHECK(Thread_Start(&thread, WaitForWatchThread, &wait));
    u32 waited = 0;
    bool consumerWaiting = false;
    while (!consumerWaiting && waited < 2000) {
      Mutex_Lock(&watch.Lock);
      consumerWaiting = watch.Waiting != 0;
      Mutex_Unlock(&watch.Lock);
      if (!consumerWaiting) Sleep_Ms(1);
      waited++;
    }
    CHECK_ALL(wait.Started != 0, consumerWaiting);
    CHECK(SessionResult_IsOk(AgentHarness_WatchRequestClose(&watch)));
    SessionResult closeWhileWaiting = AgentHarness_WatchClose(&watch);
    CHECK(closeWhileWaiting.Status == SessionStatus::Busy || SessionResult_IsOk(closeWhileWaiting));
    Thread_Join(&thread);
    CHECK(wait.Result.Status == SessionStatus::InvalidState);
    if (closeWhileWaiting.Status == SessionStatus::Busy)
      closeWhileWaiting = AgentHarness_WatchClose(&watch);
    CHECK(SessionResult_IsOk(closeWhileWaiting));
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Model/profile changes atomically replace the active request
  // configuration and release their admission reservation.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    ScriptProvider first = {};
    ScriptProvider second = {};
    AgentHarness harness = {};
    AgentHarnessConfig config = ScriptConfig(&first, nullptr, 0);
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    Model model = {};
    model.Provider = SVL("script-next");
    model.Id = SVL("model-next");
    model.ContextWindow = 200000;
    model.EffortMask = ModelEffort_Bit(ModelEffort::Medium);
    model.DefaultEffort = ModelEffort::Medium;
    ModelRequestProfile profile = {ModelEffort::Medium, 200000};
    Provider provider = ScriptProvider_Make(&second);
    CHECK(SessionResult_IsOk(AgentHarness_SetModel(&harness, model, provider, profile)));
    SessionReduction state = {};
    CHECK(SessionResult_IsOk(AgentHarness_Snapshot(&arena, &harness, &state)));
    CHECK_ALL(!harness.Running, StrView_Eq(state.Provider, model.Provider),
              StrView_Eq(state.Model, model.Id), StrView_Eq(harness.Config.ModelInfo.Id, model.Id),
              harness.Config.Stream.Profile.Effort == ModelEffort::Medium,
              harness.Config.Stream.Profile.ContextWindow == 200000);
    Model invalid = model;
    invalid.Id = SVL("invalid-profile");
    CHECK(AgentHarness_SetModel(&harness, invalid, provider,
                               ModelRequestProfile{ModelEffort::High, 200000})
              .Status == SessionStatus::InvalidArgument);
    CHECK_ALL(StrView_Eq(harness.Config.ModelInfo.Id, model.Id),
              harness.Config.Stream.Profile.Effort == ModelEffort::Medium);
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

}

static void TestHarnessToolsAndHooks() {
  // Parallel tools publish updates and finalized completions in actual finish
  // order while durable tool-result messages remain in assistant source
  // order.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));

    ExecutionCounter executions = {};
    Mutex_Init(&executions.Lock);
    Tool delay = {};
    delay.Name = SVL("delay");
    delay.Description = SVL("delay for a test");
    delay.ParamsJsonSchema = SVL(
        "{\"type\":\"object\",\"properties\":{\"label\":{\"type\":\"string\"},"
        "\"delay\":{\"type\":\"integer\",\"minimum\":0}},\"required\":"
        "[\"label\",\"delay\"],\"additionalProperties\":false}");
    delay.Ctx = &executions;
    delay.Execute = DelayExec;

    ScriptToolCall calls[2] = {};
    calls[0] = ScriptToolCall{SVL("delay"), SVL("slow"), SVL("{\"label\":\"slow\",\"delay\":40}")};
    calls[1] = ScriptToolCall{SVL("delay"), SVL("fast"), SVL("{\"label\":\"fast\",\"delay\":1}")};
    ScriptStep steps[2] = {};
    steps[0].Tools = calls;
    steps[0].ToolCount = 2;
    steps[1].Text = SVL("complete");
    steps[1].Stop = StopReason::Stop;
    ScriptProvider script = {steps, 2, 0};

    AgentHarness harness = {};
    AgentHarnessConfig config = ScriptConfig(&script, &delay, 1);
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    AgentHarnessSnapshot initial = {};
    AgentHarnessWatch* watchPtr = ArenaPushStruct(&arena, AgentHarnessWatch);
    *watchPtr = AgentHarnessWatch{};
    AgentHarnessWatch& watch = *watchPtr;
    CHECK(SessionResult_IsOk(AgentHarness_Watch(&arena, &harness, 64, &initial, &watch)));
    CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("run parallel tools"))));

    StrView liveEnds[2] = {};
    StrView durableResults[2] = {};
    u32 liveEndCount = 0;
    u32 durableResultCount = 0;
    u32 updateCount = 0;
    bool finished = false;
    while (!finished) {
      AgentHarnessEvent event = {};
      CHECK(SessionResult_IsOk(AgentHarness_WatchNext(&arena, &watch, &event)));
      if (event.Kind == AgentHarnessEventKind::TransientToolUpdate) {
        updateCount++;
      } else if (event.Kind == AgentHarnessEventKind::TransientToolEnd && liveEndCount < 2) {
        liveEnds[liveEndCount++] = StrView_Dup(&arena, event.Tool.CallId);
      } else if (event.Kind == AgentHarnessEventKind::DurableRecord) {
        if (event.Record->Kind == SessionRecordKind::Message &&
            event.Record->MessageEntry.Value.Kind == Role::ToolResult && durableResultCount < 2) {
          durableResults[durableResultCount++] =
              StrView_Dup(&arena, event.Record->MessageEntry.Value.ToolCallId);
        }
      } else if (event.Kind == AgentHarnessEventKind::TransientAgentEnd) {
        finished = true;
      }
    }
    CHECK_ALL(ExecutionCounter_Read(&executions) == 2, updateCount == 2,
              liveEndCount == 2,
              StrView_EqCStr(liveEnds[0], "fast"), StrView_EqCStr(liveEnds[1], "slow"),
              durableResultCount == 2, StrView_EqCStr(durableResults[0], "slow"),
              StrView_EqCStr(durableResults[1], "fast"));
    CHECK(SessionResult_IsOk(AgentHarness_WatchClose(&watch)));
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    Mutex_Destroy(&executions.Lock);
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Central validation rejects malformed arguments before the tool executes,
  // and unsupported custom schemas fail closed during harness initialization.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    ExecutionCounter executions = {};
    Mutex_Init(&executions.Lock);
    Tool delay = {};
    delay.Name = SVL("delay");
    delay.Description = SVL("validated delay");
    delay.ParamsJsonSchema = SVL(
        "{\"type\":\"object\",\"properties\":{\"label\":{\"type\":\"string\"},"
        "\"delay\":{\"type\":\"integer\"}},\"required\":[\"label\",\"delay\"]}");
    delay.Ctx = &executions;
    delay.Execute = DelayExec;
    ScriptToolCall invalidCalls[2] = {};
    invalidCalls[0] = ScriptToolCall{
        SVL("delay"), SVL("invalid-delay"),
        SVL("{\"label\":\"bad\",\"delay\":\"later\"}")};
    invalidCalls[1] =
        ScriptToolCall{SVL("delay"), SVL("malformed-delay"), SVL("not-json")};
    ScriptStep step = {};
    step.Tools = invalidCalls;
    step.ToolCount = 2;
    ScriptProvider script = {&step, 1, 0};
    AgentHarness harness = {};
    AgentHarnessConfig config = ScriptConfig(&script, &delay, 1);
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("validate tool"))));
    SessionReduction snapshot = {};
    CHECK(SessionResult_IsOk(AgentHarness_Snapshot(&arena, &harness, &snapshot)));
    Array<Message> context = {};
    CHECK(SessionResult_IsOk(SessionReducer_Context(&arena, &snapshot, snapshot.HeadId, &context)));
    CHECK_ALL(
        ExecutionCounter_Read(&executions) == 0, context.Count == 5,
        context.Data[2].Kind == Role::ToolResult, context.Data[2].IsError,
        StrView_Contains(context.Data[2].Content.Data[0].Text,
                         "Validation failed for tool"),
        context.Data[3].Kind == Role::ToolResult, context.Data[3].IsError,
        StrView_Contains(context.Data[3].Content.Data[0].Text,
                         "Tool arguments are invalid JSON"));
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    Mutex_Destroy(&executions.Lock);

    SessionLog emptySchemaLog = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &emptySchemaLog)));
    Tool emptySchema = delay;
    emptySchema.ParamsJsonSchema = {};
    AgentHarness emptySchemaHarness = {};
    AgentHarnessConfig emptySchemaConfig = ScriptConfig(&script, &emptySchema, 1);
    CHECK(SessionResult_IsOk(
        AgentHarness_Init(&emptySchemaHarness, emptySchemaLog, &emptySchemaConfig)));
    CHECK(SessionResult_IsOk(AgentHarness_Close(&emptySchemaHarness)));

    SessionLog invalidLog = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &invalidLog)));
    Tool unsupported = delay;
    unsupported.ParamsJsonSchema = SVL("{\"type\":\"string\",\"pattern\":\"x\"}");
    AgentHarness invalidHarness = {};
    AgentHarnessConfig invalidConfig = ScriptConfig(&script, &unsupported, 1);
    CHECK(AgentHarness_Init(&invalidHarness, invalidLog, &invalidConfig).Status ==
          SessionStatus::InvalidArgument);
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Awaited hooks may alter accepted input, request context, provider
  // settings, and tool arguments; all effect-facing values are applied
  // before tools run, and every appended record is still a plain Message.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    Tool echo = {};
    echo.Name = SVL("echo");
    echo.Description = SVL("echo arguments");
    echo.ParamsJsonSchema = SVL("{\"type\":\"object\"}");
    echo.Execute = EchoExec;
    ScriptStep steps[2] = {};
    ScriptToolCall hookCall[1] = {ScriptToolCall{SVL("echo"), SVL("hook-call"), SVL("{}")}};
    steps[0].Tools = hookCall;
    steps[0].ToolCount = 1;
    steps[1].Text = SVL("hooked run complete");
    steps[1].Stop = StopReason::Stop;
    ScriptProvider script = {steps, 2, 0};
    HarnessHookState hookState = {};
    AgentHarness harness = {};
    AgentHarnessConfig config = ScriptConfig(&script, &echo, 1);
    config.Hooks.User = &hookState;
    config.Hooks.BeforeRun = HookBeforeRun;
    config.Hooks.BeforeContext = HookBeforeContext;
    config.Hooks.BeforeProvider = HookBeforeProvider;
    config.Hooks.AfterProvider = HookAfterProvider;
    config.Hooks.BeforeTool = HookBeforeTool;
    config.Hooks.AfterTurn = HookAfterTurn;
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("original prompt"))));

    SessionReduction snapshot = {};
    CHECK(SessionResult_IsOk(AgentHarness_Snapshot(&arena, &harness, &snapshot)));
    Array<Message> context = {};
    CHECK(SessionResult_IsOk(SessionReducer_Context(&arena, &snapshot, snapshot.HeadId, &context)));
    CHECK_ALL(context.Count == 4,
              ContextTextEq(&context, 0, Role::User, "hooked durable prompt"),
              ContextTextEq(&context, 2, Role::ToolResult, "echo:{\"hooked\":true}"),
              ContextTextEq(&context, 3, Role::Assistant, "hooked run complete"),
              hookState.BeforeRunCount == 1, hookState.BeforeContextCount == 2,
              hookState.BeforeProviderCount == 2, hookState.AfterProviderCount == 2,
              hookState.BeforeToolCount == 1, hookState.AfterTurnCount == 1,
              hookState.FirstContextCount == 2, hookState.LastContextCount == 4);

    Array<SessionRecord> records = {};
    CHECK(SessionResult_IsOk(SessionLog_ReadAll(&harness.Log, &arena, &records)));
    CHECK(AllNonRootAreMessages(&records));
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // A successful provider turn followed by an AfterTurn hook failure must
  // publish the final agent lifecycle event as failed, matching its result.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    ScriptStep step = {};
    step.Text = SVL("completed before hook");
    step.Stop = StopReason::Stop;
    ScriptProvider script = {&step, 1, 0};
    AgentHarnessConfig config = ScriptConfig(&script, nullptr, 0);
    config.Hooks.AfterTurn = FailAfterTurn;
    AgentHarness harness = {};
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    AgentHarnessSnapshot initial = {};
    AgentHarnessWatch* watchPtr = ArenaPushStruct(&arena, AgentHarnessWatch);
    *watchPtr = AgentHarnessWatch{};
    AgentHarnessWatch& watch = *watchPtr;
    CHECK(SessionResult_IsOk(
        AgentHarness_Watch(&arena, &harness, 16, &initial, &watch)));
    CHECK(AgentHarness_Run(&harness, SVL("go")).Status ==
          SessionStatus::HookError);
    WatchTally tally = DrainUntilAgentEnd(&arena, &watch);
    CHECK_ALL(tally.AgentEnd == 1,
              tally.AgentEndOutcome == AgentTurnOutcome::Failed,
              tally.AgentEndErrorMentionsHook);
    CHECK(SessionResult_IsOk(AgentHarness_WatchClose(&watch)));
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Hook failure becomes a typed HookError while still completing the
  // assistant/tool-result group -- with a normal error tool result recorded
  // -- without executing the blocked tool effect.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    u32 executions = 0;
    Tool tool = {};
    tool.Name = SVL("blocked");
    tool.Description = SVL("must not execute");
    tool.ParamsJsonSchema = SVL("{\"type\":\"object\"}");
    tool.Ctx = &executions;
    tool.Execute = CountExec;
    ScriptStep step = {};
    ScriptToolCall blockedCall[1] = {
        ScriptToolCall{SVL("blocked"), SVL("blocked-call"), SVL("{}")}};
    step.Tools = blockedCall;
    step.ToolCount = 1;
    ScriptProvider script = {&step, 1, 0};
    u32 hookCalls = 0;
    AgentHarness harness = {};
    AgentHarnessConfig config = ScriptConfig(&script, &tool, 1);
    config.Hooks.User = &hookCalls;
    config.Hooks.BeforeTool = FailBeforeTool;
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    CHECK(AgentHarness_Run(&harness, SVL("block tool")).Status == SessionStatus::HookError);
    SessionReduction snapshot = {};
    CHECK(SessionResult_IsOk(AgentHarness_Snapshot(&arena, &harness, &snapshot)));
    Array<Message> context = {};
    CHECK(SessionResult_IsOk(SessionReducer_Context(&arena, &snapshot, snapshot.HeadId, &context)));
    Array<SessionRecord> records = {};
    CHECK(SessionResult_IsOk(SessionLog_ReadAll(&harness.Log, &arena, &records)));
    CHECK_ALL(hookCalls == 1, executions == 0, context.Count == 3,
              context.Data[2].Kind == Role::ToolResult, context.Data[2].IsError,
              ContextTextEq(&context, 2, Role::ToolResult, "tool hook failed"),
              AllNonRootAreMessages(&records), records.Count == 4);
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Steering queued (memory-only) while a tool effect is active folds onto
  // the tree right after the tool-result batch, before the next provider
  // request -- with no durable queue record anywhere.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));

    Tool steer = {};
    steer.Name = SVL("steer");
    steer.Description = SVL("queue steering");
    steer.ParamsJsonSchema = SVL("{\"type\":\"object\"}");
    steer.Execute = SteerExec;
    ScriptStep steps[2] = {};
    ScriptToolCall steerCall[1] = {ScriptToolCall{SVL("steer"), SVL("call-steer"), SVL("{}")}};
    steps[0].Tools = steerCall;
    steps[0].ToolCount = 1;
    steps[1].Text = SVL("after steering");
    steps[1].Stop = StopReason::Stop;
    ScriptProvider script = {steps, 2, 0};

    AgentHarness harness = {};
    steer.Ctx = &harness;
    AgentHarnessConfig config = ScriptConfig(&script, &steer, 1);
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    usize queuedBaseline = harness.Queued.Used;
    CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("start"))));
    CHECK(harness.Queued.Used == queuedBaseline);

    SessionReduction snapshot = {};
    CHECK(SessionResult_IsOk(AgentHarness_Snapshot(&arena, &harness, &snapshot)));
    Array<Message> context = {};
    CHECK(SessionResult_IsOk(SessionReducer_Context(&arena, &snapshot, snapshot.HeadId, &context)));
    CHECK_ALL(context.Count == 5, context.Data[1].Kind == Role::Assistant,
              ContextTextEq(&context, 2, Role::ToolResult, "queued"),
              ContextTextEq(&context, 3, Role::User, "steered durably"),
              ContextTextEq(&context, 4, Role::Assistant, "after steering"));

    Array<SessionRecord> records = {};
    CHECK(SessionResult_IsOk(SessionLog_ReadAll(&harness.Log, &arena, &records)));
    CHECK(AllNonRootAreMessages(&records));
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

}

static void TestHarnessPerTurnHooks() {
  // ShouldStopAfterTurn ends the run gracefully after the requested turn: the
  // assistant message and its tool results are still durable, but no further
  // provider request starts and neither steering source is polled.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    Tool echo = {};
    echo.Name = SVL("echo");
    echo.Description = SVL("echo arguments");
    echo.ParamsJsonSchema = SVL("{\"type\":\"object\"}");
    echo.Execute = EchoExec;
    ScriptStep steps[2] = {};
    ScriptToolCall call[1] = {ScriptToolCall{SVL("echo"), SVL("stop-call"), SVL("{}")}};
    steps[0].Tools = call;
    steps[0].ToolCount = 1;
    steps[1].Text = SVL("must never run");
    steps[1].Stop = StopReason::Stop;
    ScriptProvider script = {steps, 2, 0};

    TurnHookState hooks = {};
    hooks.StopAfterTurn = 1;
    hooks.SteeringBudget = 4;
    AgentHarness harness = {};
    AgentHarnessConfig config = ScriptConfig(&script, &echo, 1);
    config.Hooks.User = &hooks;
    config.Hooks.PrepareNextTurn = HookPrepareNextTurn;
    config.Hooks.ShouldStopAfterTurn = HookShouldStopAfterTurn;
    config.Hooks.GetSteeringMessages = HookSteeringMessages;
    config.Hooks.GetFollowUpMessages = HookFollowUpMessages;
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("stop after one turn"))));

    SessionReduction snapshot = {};
    CHECK(SessionResult_IsOk(AgentHarness_Snapshot(&arena, &harness, &snapshot)));
    Array<Message> context = {};
    CHECK(SessionResult_IsOk(SessionReducer_Context(&arena, &snapshot, snapshot.HeadId, &context)));
    // prompt + run-start steering + assistant + tool result. The only steering
    // poll is the one before the first turn; the post-turn checkpoint is
    // skipped entirely by the graceful stop.
    CHECK_ALL(script.Index == 1, hooks.PrepareCount == 1, hooks.ShouldStopCount == 1,
              hooks.SteeringCount == 1, hooks.FollowUpCount == 0, context.Count == 4,
              ContextTextEq(&context, 1, Role::User, "hook steering"),
              context.Data[2].Kind == Role::Assistant,
              context.Data[3].Kind == Role::ToolResult);
    // The turn context reports the finished turn, not a partial one.
    CHECK_ALL(hooks.LastAssistantSeen, hooks.LastToolResults == 1,
              hooks.LastContextCount == 4, hooks.LastNewMessages == 4);

    Array<SessionRecord> records = {};
    CHECK(SessionResult_IsOk(SessionLog_ReadAll(&harness.Log, &arena, &records)));
    CHECK(AllNonRootAreMessages(&records));
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // PrepareNextTurn's model and effort override applies to every later turn of
  // the same run, and never leaks into AgentHarnessConfig.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    Tool echo = {};
    echo.Name = SVL("echo");
    echo.Description = SVL("echo arguments");
    echo.ParamsJsonSchema = SVL("{\"type\":\"object\"}");
    echo.Execute = EchoExec;
    ScriptStep steps[3] = {};
    ScriptToolCall first[1] = {ScriptToolCall{SVL("echo"), SVL("a"), SVL("{}")}};
    ScriptToolCall second[1] = {ScriptToolCall{SVL("echo"), SVL("b"), SVL("{}")}};
    steps[0].Tools = first;
    steps[0].ToolCount = 1;
    steps[1].Tools = second;
    steps[1].ToolCount = 1;
    steps[2].Text = SVL("done");
    steps[2].Stop = StopReason::Stop;
    ScriptProvider script = {steps, 3, 0};

    TurnHookState hooks = {};
    hooks.OverrideNextTurn = true;
    AgentHarness harness = {};
    AgentHarnessConfig config = ScriptConfig(&script, &echo, 1);
    config.Hooks.User = &hooks;
    config.Hooks.PrepareNextTurn = HookPrepareNextTurn;
    config.Hooks.BeforeProvider = HookObserveProvider;
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("override the model"))));

    CHECK_ALL(script.Index == 3, hooks.PrepareCount == 3, hooks.ProviderTurnCount == 3,
              !hooks.ProviderSawOverride[0], hooks.ProviderSawOverride[1],
              hooks.ProviderSawOverride[2],
              hooks.ProviderEffort[0] == ModelEffort::None,
              hooks.ProviderEffort[1] == ModelEffort::Medium,
              hooks.ProviderEffort[2] == ModelEffort::Medium);
    // The harness config itself is untouched: overrides are run-scoped only.
    CHECK_ALL(StrView_EqCStr(harness.Config.ModelInfo.Id, "script-model"),
              harness.Config.Stream.Profile.Effort == ModelEffort::None);
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // A PrepareNextTurn context override replaces the branch context but stays
  // live: later turns accumulate their own assistant message and tool results
  // onto it. A frozen snapshot would re-send an identical request every turn,
  // which a non-terminating tool batch turns into an unbounded loop.
  // ShouldStopAfterTurn observes the override, not the pre-override context.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    Tool echo = {};
    echo.Name = SVL("echo");
    echo.Description = SVL("echo arguments");
    echo.ParamsJsonSchema = SVL("{\"type\":\"object\"}");
    echo.Execute = EchoExec;
    ScriptStep steps[3] = {};
    ScriptToolCall first[1] = {ScriptToolCall{SVL("echo"), SVL("a"), SVL("{}")}};
    ScriptToolCall second[1] = {ScriptToolCall{SVL("echo"), SVL("b"), SVL("{}")}};
    steps[0].Tools = first;
    steps[0].ToolCount = 1;
    steps[1].Tools = second;
    steps[1].ToolCount = 1;
    steps[2].Text = SVL("done");
    steps[2].Stop = StopReason::Stop;
    ScriptProvider script = {steps, 3, 0};

    TurnHookState hooks = {};
    hooks.OverrideContext = true;
    AgentHarness harness = {};
    AgentHarnessConfig config = ScriptConfig(&script, &echo, 1);
    config.Hooks.User = &hooks;
    config.Hooks.PrepareNextTurn = HookPrepareNextTurn;
    config.Hooks.ShouldStopAfterTurn = HookShouldStopAfterTurn;
    config.Hooks.BeforeProvider = HookObserveProvider;
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("override the context"))));

    // Turn 1 sees the real branch (just the prompt). Turn 2 sees the 1-message
    // override installed after turn 1. Turn 3 sees that override plus turn 2's
    // own assistant message and tool result.
    CHECK_ALL(script.Index == 3, hooks.ProviderTurnCount == 3,
              hooks.ProviderMessageCount[0] == 1, hooks.ProviderMessageCount[1] == 1,
              hooks.ProviderMessageCount[2] == 3);
    // The stop hook sees the post-override context on the turn that installed
    // it (1), then the accumulated one: +assistant +tool result (3), then the
    // final text-only turn's assistant (4). Never the growing real branch.
    CHECK_ALL(hooks.StopSeenCount == 3, hooks.StopSeenContext[0] == 1,
              hooks.StopSeenContext[1] == 3, hooks.StopSeenContext[2] == 4);

    // The durable branch is untouched by the override: prompt + 2x(assistant,
    // tool result) + final assistant.
    SessionReduction snapshot = {};
    CHECK(SessionResult_IsOk(AgentHarness_Snapshot(&arena, &harness, &snapshot)));
    Array<Message> context = {};
    CHECK(SessionResult_IsOk(SessionReducer_Context(&arena, &snapshot, snapshot.HeadId, &context)));
    CHECK_ALL(context.Count == 6, ContextTextEq(&context, 0, Role::User, "override the context"),
              ContextTextEq(&context, 5, Role::Assistant, "done"));
    Array<SessionRecord> records = {};
    CHECK(SessionResult_IsOk(SessionLog_ReadAll(&harness.Log, &arena, &records)));
    CHECK(AllNonRootAreMessages(&records));
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // A message from the in-memory queue must reach a live context override too,
  // not just a hook-sourced one -- otherwise a user's steering/follow-up is
  // durably recorded but invisible to the model.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    ScriptStep steps[2] = {};
    steps[0].Text = SVL("first");
    steps[0].Stop = StopReason::Stop;
    steps[1].Text = SVL("second");
    steps[1].Stop = StopReason::Stop;
    ScriptProvider script = {steps, 2, 0};

    TurnHookState hooks = {};
    hooks.OverrideContext = true;
    AgentHarness harness = {};
    AgentHarnessConfig config = ScriptConfig(&script, nullptr, 0);
    config.Hooks.User = &hooks;
    config.Hooks.PrepareNextTurn = HookPrepareNextTurn;
    config.Hooks.BeforeProvider = HookObserveProvider;
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    CHECK(SessionResult_IsOk(AgentHarness_QueueFollowUp(&harness, SVL("queued follow-up"))));
    CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("start"))));

    // Turn 1 sends the real branch (the prompt). The override installed after
    // turn 1 holds 1 message; the queued follow-up drains onto it, so turn 2
    // sends 2. A queue drain that bypassed the accumulator would send 1.
    CHECK_ALL(script.Index == 2, hooks.ProviderTurnCount == 2,
              hooks.ProviderMessageCount[0] == 1, hooks.ProviderMessageCount[1] == 2);

    SessionReduction snapshot = {};
    CHECK(SessionResult_IsOk(AgentHarness_Snapshot(&arena, &harness, &snapshot)));
    Array<Message> context = {};
    CHECK(SessionResult_IsOk(SessionReducer_Context(&arena, &snapshot, snapshot.HeadId, &context)));
    CHECK_ALL(context.Count == 4, ContextTextEq(&context, 2, Role::User, "queued follow-up"),
              ContextTextEq(&context, 3, Role::Assistant, "second"));
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // The hook message sources drive the same two checkpoints the in-memory
  // queues do: steering continues the turn loop, follow-up restarts a run that
  // would otherwise have stopped.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    ScriptStep steps[3] = {};
    steps[0].Text = SVL("first");
    steps[0].Stop = StopReason::Stop;
    steps[1].Text = SVL("second");
    steps[1].Stop = StopReason::Stop;
    steps[2].Text = SVL("third");
    steps[2].Stop = StopReason::Stop;
    ScriptProvider script = {steps, 3, 0};

    TurnHookState hooks = {};
    hooks.SteeringBudget = 1;   // consumed by the pre-first-turn poll
    hooks.FollowUpBudget = 1;   // one extra run turn after the agent would stop
    AgentHarness harness = {};
    AgentHarnessConfig config = ScriptConfig(&script, nullptr, 0);
    config.Hooks.User = &hooks;
    config.Hooks.GetSteeringMessages = HookSteeringMessages;
    config.Hooks.GetFollowUpMessages = HookFollowUpMessages;
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("start"))));

    SessionReduction snapshot = {};
    CHECK(SessionResult_IsOk(AgentHarness_Snapshot(&arena, &harness, &snapshot)));
    Array<Message> context = {};
    CHECK(SessionResult_IsOk(SessionReducer_Context(&arena, &snapshot, snapshot.HeadId, &context)));
    CHECK_ALL(script.Index == 2, context.Count == 5,
              ContextTextEq(&context, 0, Role::User, "start"),
              ContextTextEq(&context, 1, Role::User, "hook steering"),
              ContextTextEq(&context, 2, Role::Assistant, "first"),
              ContextTextEq(&context, 3, Role::User, "hook follow-up"),
              ContextTextEq(&context, 4, Role::Assistant, "second"));
    // Steering is polled before turn 1 and after each of the 2 turns;
    // follow-up only at the two points the run would have stopped.
    CHECK_ALL(hooks.SteeringCount == 3, hooks.FollowUpCount == 2,
              hooks.SteeringBudget == 0, hooks.FollowUpBudget == 0);

    Array<SessionRecord> records = {};
    CHECK(SessionResult_IsOk(SessionLog_ReadAll(&harness.Log, &arena, &records)));
    CHECK(AllNonRootAreMessages(&records));
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // ConvertToLlm is the last rewrite before the request is built: it runs
  // after BeforeContext, and only its output reaches the provider.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    ScriptStep steps[1] = {};
    steps[0].Text = SVL("converted");
    steps[0].Stop = StopReason::Stop;
    ScriptProvider script = {steps, 1, 0};

    TurnHookState hooks = {};
    AgentHarness harness = {};
    AgentHarnessConfig config = ScriptConfig(&script, nullptr, 0);
    config.Hooks.User = &hooks;
    config.Hooks.BeforeContext = HookCountBeforeContext;
    config.Hooks.ConvertToLlm = HookConvertToLlm;
    config.Hooks.BeforeProvider = HookObserveProvider;
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("only prompt"))));

    // 1 durable prompt -> BeforeContext appends 1 -> ConvertToLlm sees 2 and
    // appends 1 -> the provider request carries 3. Neither marker is durable.
    CHECK_ALL(hooks.BeforeContextCount == 1, hooks.ConvertCount == 1,
              hooks.BeforeContextLastCount == 2, hooks.ConvertLastCount == 2,
              hooks.ProviderTurnCount == 1, hooks.ProviderMessageCount[0] == 3);

    SessionReduction snapshot = {};
    CHECK(SessionResult_IsOk(AgentHarness_Snapshot(&arena, &harness, &snapshot)));
    Array<Message> context = {};
    CHECK(SessionResult_IsOk(SessionReducer_Context(&arena, &snapshot, snapshot.HeadId, &context)));
    CHECK_ALL(context.Count == 2, ContextTextEq(&context, 0, Role::User, "only prompt"),
              ContextTextEq(&context, 1, Role::Assistant, "converted"));
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Both new hook families surface a typed HookError, and a failing hook stops
  // the run at its own checkpoint.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    ScriptStep steps[2] = {};
    steps[0].Text = SVL("first");
    steps[0].Stop = StopReason::Stop;
    steps[1].Text = SVL("never");
    steps[1].Stop = StopReason::Stop;
    ScriptProvider script = {steps, 2, 0};

    u32 prepareCalls = 0;
    AgentHarness harness = {};
    AgentHarnessConfig config = ScriptConfig(&script, nullptr, 0);
    config.Hooks.User = &prepareCalls;
    config.Hooks.PrepareNextTurn = FailPrepareNextTurn;
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    CHECK(AgentHarness_Run(&harness, SVL("fail after turn")).Status == SessionStatus::HookError);
    CHECK_ALL(prepareCalls == 1, script.Index == 1);
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    ScriptStep steps[1] = {};
    steps[0].Text = SVL("never reached");
    steps[0].Stop = StopReason::Stop;
    ScriptProvider script = {steps, 1, 0};

    u32 steeringCalls = 0;
    AgentHarness harness = {};
    AgentHarnessConfig config = ScriptConfig(&script, nullptr, 0);
    config.Hooks.User = &steeringCalls;
    config.Hooks.GetSteeringMessages = FailSteeringMessages;
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    // The run-start poll fails before the first provider request.
    CHECK(AgentHarness_Run(&harness, SVL("fail steering")).Status == SessionStatus::HookError);
    CHECK_ALL(steeringCalls == 1, script.Index == 0);
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }
}

static void TestHarnessCancellationAndBranches() {
  // Append failure faults the harness before any provider effect starts.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    ScriptStep step = {};
    step.Text = SVL("must not run");
    ScriptProvider script = {&step, 1, 0};
    AgentHarness harness = {};
    AgentHarnessConfig config = ScriptConfig(&script, nullptr, 0);
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    MemoryRepository_FailAfterAppends(&memory, 0);
    CHECK(AgentHarness_Run(&harness, SVL("fail before effect")).Status == SessionStatus::IoError);
    CHECK_ALL(script.Index == 0, harness.Faulted);
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Cancellation accepted after an assistant tool call is durable but before
  // dispatch materializes an error result without invoking the tool.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));

    u32 executions = 0;
    Tool echo = {};
    echo.Name = SVL("echo");
    echo.Description = SVL("must not execute after cancellation");
    echo.ParamsJsonSchema = SVL("{\"type\":\"object\"}");
    echo.Ctx = &executions;
    echo.Execute = EchoExec;

    ScriptStep step = {};
    ScriptToolCall cancelCall[1] = {
        ScriptToolCall{SVL("echo"), SVL("cancel-before-dispatch"), SVL("{}")}};
    step.Tools = cancelCall;
    step.ToolCount = 1;
    ScriptProvider script = {&step, 1, 0};
    BlockingBeforeToolState hook = {};
    AgentHarness harness = {};
    AgentHarnessConfig config = ScriptConfig(&script, &echo, 1);
    config.Hooks.User = &hook;
    config.Hooks.BeforeTool = BlockingBeforeTool;
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));

    HarnessRunThread run = {};
    run.Harness = &harness;
    run.Prompt = SVL("cancel before dispatch");
    Thread thread = {};
    CHECK(Thread_Start(&thread, RunHarnessThread, &run));
    u32 waited = 0;
    while (!hook.Entered && waited < 2000) {
      Sleep_Ms(1);
      waited++;
    }
    CHECK(hook.Entered != 0);
    CHECK(SessionResult_IsOk(AgentHarness_RequestCancel(&harness)));
    hook.Release = 1;
    Thread_Join(&thread);
    CHECK_ALL(run.Result.Status == SessionStatus::Cancelled,
              executions == 0);

    SessionReduction snapshot = {};
    CHECK(SessionResult_IsOk(
        AgentHarness_Snapshot(&arena, &harness, &snapshot)));
    Array<Message> context = {};
    CHECK(SessionResult_IsOk(SessionReducer_Context(
        &arena, &snapshot, snapshot.HeadId, &context)));
    CHECK_ALL(
        context.Count == 3, context.Data[2].Kind == Role::ToolResult,
        context.Data[2].IsError,
        StrView_Contains(context.Data[2].Content.Data[0].Text,
                         "Tool execution cancelled"));
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Cancellation completeness: every live tool call still receives exactly
  // one normal result before the harness stops -- cancellation mid-batch
  // never leaves a dispatched call without a result -- and no further
  // provider request happens afterward.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));

    ExecutionCounter executions = {};
    Mutex_Init(&executions.Lock);
    Tool delay = {};
    delay.Name = SVL("delay");
    delay.Description = SVL("delay for a test");
    delay.ParamsJsonSchema = SVL(
        "{\"type\":\"object\",\"properties\":{\"label\":{\"type\":\"string\"},"
        "\"delay\":{\"type\":\"integer\"}},\"required\":[\"label\",\"delay\"]}");
    delay.Ctx = &executions;
    delay.Execute = DelayExec;

    CountingToolCallProviderState providerState = {};
    AgentHarnessConfig config = {};
    config.SystemPrompt = SVL("cancel mid batch");
    config.ModelInfo.Id = SVL("counting-model");
    config.ModelInfo.Provider = SVL("test");
    config.Tools = &delay;
    config.ToolCount = 1;
    config.ProviderImpl.Ctx = &providerState;
    config.ProviderImpl.Stream = CountingToolCallProviderStream;
    config.ToolExec = ToolExecution::Parallel;
    AgentHarness harness = {};
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));

    HarnessRunThread run = {};
    run.Harness = &harness;
    run.Prompt = SVL("run then cancel mid-batch");
    Thread thread = {};
    CHECK(Thread_Start(&thread, RunHarnessThread, &run));
    u32 waited = 0;
    while (ExecutionCounter_Read(&executions) == 0 && waited < 2000) {
      Sleep_Ms(1);
      waited++;
    }
    CHECK(SessionResult_IsOk(AgentHarness_RequestCancel(&harness)));
    Thread_Join(&thread);
    CHECK(run.Result.Status == SessionStatus::Cancelled);

    Array<SessionRecord> records = {};
    CHECK(SessionResult_IsOk(SessionLog_ReadAll(&harness.Log, &arena, &records)));
    u32 toolResultCount = 0;
    for (u32 i = 0; i < records.Count; i++)
      if (records.Data[i].Kind == SessionRecordKind::Message &&
          records.Data[i].MessageEntry.Value.Kind == Role::ToolResult)
        toolResultCount++;
    u32 executionCount = ExecutionCounter_Read(&executions);
    CHECK_ALL(executionCount >= 1, executionCount <= 2,
              toolResultCount == 2, providerState.Calls == 1);
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    Mutex_Destroy(&executions.Lock);
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Cancellation accepted while an after-turn hook is still finalizing wins
  // the locked completion race and produces one aborted terminal event.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    ScriptStep step = {};
    step.Text = SVL("completed response");
    step.Stop = StopReason::Stop;
    ScriptProvider script = {&step, 1, 0};
    BlockingBeforeToolState hook = {};
    AgentHarnessConfig config = ScriptConfig(&script, nullptr, 0);
    config.Hooks.User = &hook;
    config.Hooks.AfterTurn = BlockingAfterTurn;
    AgentHarness harness = {};
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    AgentHarnessSnapshot initial = {};
    AgentHarnessWatch* watchPtr =
        ArenaPushStruct(&arena, AgentHarnessWatch);
    *watchPtr = AgentHarnessWatch{};
    AgentHarnessWatch& watch = *watchPtr;
    CHECK(SessionResult_IsOk(
        AgentHarness_Watch(&arena, &harness, 32, &initial, &watch)));

    HarnessRunThread run = {};
    run.Harness = &harness;
    run.Prompt = SVL("cancel during finalization");
    Thread thread = {};
    CHECK(Thread_Start(&thread, RunHarnessThread, &run));
    u32 waited = 0;
    while (!hook.Entered && waited < 2000) {
      Sleep_Ms(1);
      waited++;
    }
    CHECK(hook.Entered != 0);
    CHECK(SessionResult_IsOk(AgentHarness_RequestCancel(&harness)));
    hook.Release = 1;
    Thread_Join(&thread);
    CHECK(run.Result.Status == SessionStatus::Cancelled);

    WatchTally tally = DrainUntilAgentEnd(&arena, &watch);
    CHECK_ALL(tally.CancelRequested == 1, tally.AgentEnd == 1,
              tally.AgentEndOutcome == AgentTurnOutcome::Aborted);
    CHECK(SessionResult_IsOk(AgentHarness_WatchClose(&watch)));
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // An empty aborted assistant remains in the durable transcript but is not
  // replayed to the provider when the user continues the conversation.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    AbortContinueProviderState providerState = {};
    AgentHarnessConfig config = {};
    config.SystemPrompt = SVL("abort continuation");
    config.ModelInfo.Id = SVL("abort-model");
    config.ModelInfo.Provider = SVL("test");
    config.ProviderImpl.Ctx = &providerState;
    config.ProviderImpl.Stream = AbortContinueProviderStream;
    config.ToolExec = ToolExecution::Parallel;
    AgentHarness harness = {};
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    HarnessRunThread first = {};
    first.Harness = &harness;
    first.Prompt = SVL("cancel me");
    Thread thread = {};
    CHECK(Thread_Start(&thread, RunHarnessThread, &first));
    u32 waited = 0;
    while (!providerState.Started && waited < 2000) {
      Sleep_Ms(1);
      waited++;
    }
    CHECK(providerState.Started != 0);
    CHECK(SessionResult_IsOk(AgentHarness_RequestCancel(&harness)));
    Thread_Join(&thread);
    CHECK(first.Result.Status == SessionStatus::Cancelled);
    CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("after cancel"))));

    AgentHarnessSnapshot snapshot = {};
    CHECK(SessionResult_IsOk(AgentHarness_SnapshotFull(&arena, &harness, &snapshot)));
    CHECK_ALL(providerState.Calls == 2, providerState.FollowUpContextValid,
              snapshot.Transcript.Count == 4,
              ContextTextEq(&snapshot.Transcript, 0, Role::User, "cancel me"),
              snapshot.Transcript.Data[1].Kind == Role::Assistant,
              snapshot.Transcript.Data[1].Stop == StopReason::Aborted,
              snapshot.Transcript.Data[1].Content.Count == 0,
              ContextTextEq(&snapshot.Transcript, 2, Role::User, "after cancel"),
              ContextTextEq(&snapshot.Transcript, 3, Role::Assistant, "continued"));
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Restart: a fresh harness Init over a log left with an orphaned assistant
  // tool call (simulating a crash before the tool's result landed) never
  // suspends or asks for reconciliation -- it just loads normally. The next
  // provider request transiently synthesizes "No result provided" for the
  // orphaned call; the synthetic result is never persisted.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    Array<SessionRecord> root = {};
    CHECK(SessionResult_IsOk(SessionLog_ReadAll(&log, &arena, &root)));

    SessionRecord userRecord = {};
    userRecord.EntryId = DurableId_Generate();
    userRecord.Kind = SessionRecordKind::Message;
    userRecord.MessageEntry.ParentId = root.Data[0].EntryId;
    userRecord.MessageEntry.Value = Message_User(&arena, SVL("before crash"));

    Message assistantMsg = Message_AssistantEmpty(&arena);
    assistantMsg.Stop = StopReason::ToolUse;
    Array_Push(&assistantMsg.Content, Content_ToolCall(SVL("call-orphan"), SVL("read"), SVL("{}")));
    SessionRecord assistantRecord = {};
    assistantRecord.EntryId = DurableId_Generate();
    assistantRecord.Kind = SessionRecordKind::Message;
    assistantRecord.MessageEntry.ParentId = userRecord.EntryId;
    assistantRecord.MessageEntry.Value = assistantMsg;

    SessionRecord seed[2] = {userRecord, assistantRecord};
    CHECK(SessionResult_IsOk(SessionLog_AppendBatch(&log, seed, 2)));
    DurableId sessionId = root.Data[0].SessionId;
    StrView projectKey = root.Data[0].SessionStarted.ProjectKey;
    SessionLog_Close(&log);

    SessionOpenOptions openOptions = {};
    openOptions.SessionId = sessionId;
    openOptions.ProjectKey = projectKey;
    openOptions.Writable = true;
    SessionLog reopened = {};
    SessionInfo reopenedInfo = {};
    CHECK(SessionResult_IsOk(
        repository.Open(repository.Ctx, &arena, &openOptions, &reopened, &reopenedInfo)));

    OrphanCaptureState providerState = {};
    AgentHarnessConfig config = {};
    config.SystemPrompt = SVL("restart");
    config.ModelInfo.Id = SVL("restart-model");
    config.ModelInfo.Provider = SVL("test");
    config.ProviderImpl.Ctx = &providerState;
    config.ProviderImpl.Stream = OrphanCaptureStream;
    AgentHarness harness = {};
    u64 baseline = memory.ReadAllCalls;
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, reopened, &config)));
    CHECK_ALL(memory.ReadAllCalls == baseline + 1, !harness.Faulted);

    CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("next"))));
    CHECK(memory.ReadAllCalls == baseline + 1);
    CHECK_ALL(providerState.CapturedCount == 4, providerState.Captured[0].Kind == Role::User,
              providerState.Captured[1].Kind == Role::Assistant,
              providerState.Captured[2].Kind == Role::ToolResult, providerState.Captured[2].IsError,
              StrView_EqCStr(providerState.Captured[2].Content.Data[0].Text, "No result provided"),
              StrView_EqCStr(providerState.Captured[2].ToolCallId, "call-orphan"),
              providerState.Captured[3].Kind == Role::User);

    Array<SessionRecord> after = {};
    CHECK(SessionResult_IsOk(SessionLog_ReadAll(&harness.Log, &arena, &after)));
    bool foundSynthetic = false;
    for (u32 i = 0; i < after.Count; i++) {
      if (after.Data[i].Kind != SessionRecordKind::Message) continue;
      const Message& value = after.Data[i].MessageEntry.Value;
      for (u32 c = 0; c < value.Content.Count; c++)
        if (StrView_Contains(value.Content.Data[c].Text, "No result provided")) foundSynthetic = true;
    }
    CHECK(!foundSynthetic);
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Compaction appends exactly one Compaction entry carrying its own usage,
  // replaces prior context with the summary, and clears provider context.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    ScriptStep steps[2] = {};
    steps[0].Text = SVL("answer before compaction");
    steps[0].Stop = StopReason::Stop;
    steps[1].Text = SVL("compact summary");
    steps[1].Stop = StopReason::Stop;
    ScriptProvider script = {steps, 2, 0};
    AgentHarness harness = {};
    AgentHarnessConfig config = ScriptConfig(&script, nullptr, 0);
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("old prompt"))));
    CHECK(SessionResult_IsOk(AgentHarness_Compact(&harness)));

    SessionReduction snapshot = {};
    CHECK(SessionResult_IsOk(AgentHarness_Snapshot(&arena, &harness, &snapshot)));
    Array<Message> context = {};
    CHECK(SessionResult_IsOk(SessionReducer_Context(&arena, &snapshot, snapshot.HeadId, &context)));
    CHECK_ALL(context.Count == 1,
              ContextTextEq(&context, 0, Role::User,
                            "Summary of previous conversation:\ncompact summary"));

    Array<SessionRecord> records = {};
    CHECK(SessionResult_IsOk(SessionLog_ReadAll(&harness.Log, &arena, &records)));
    const SessionRecord* compactionRecord = nullptr;
    for (u32 i = 0; i < records.Count; i++)
      if (records.Data[i].Kind == SessionRecordKind::Compaction) compactionRecord = &records.Data[i];
    CHECK(compactionRecord != nullptr);
    if (compactionRecord) {
      CHECK_ALL(compactionRecord->CompactionEntry.HasUsage,
                DurableId_Eq(compactionRecord->CompactionEntry.BaseEntryId, records.Data[0].EntryId));
    }
    CHECK(CountKind(&records, SessionRecordKind::Compaction) == 1);

    SessionMetadataBuildOptions metadataOptions = {};
    SessionInfo metadata = {};
    CHECK(SessionResult_IsOk(
        SessionMetadata_Build(&arena, records.Data, records.Count, &metadataOptions, &metadata)));
    CHECK_ALL(metadata.CompactionCount == 1, metadata.MessageCount == 2);

    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Navigation restores ancestry-derived settings, the next run branches from
  // the checked-out head, and a fork of a labeled branch carries the whole
  // ancestry (model/thinking/tools/message/branch-label) across.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    ScriptStep step = {};
    step.Text = SVL("branched response");
    step.Stop = StopReason::Stop;
    ScriptProvider script = {&step, 1, 0};
    AgentHarness harness = {};
    AgentHarnessConfig config = ScriptConfig(&script, nullptr, 0);
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));

    SessionReduction initial = {};
    CHECK(SessionResult_IsOk(AgentHarness_Snapshot(&arena, &harness, &initial)));
    DurableId rootId = initial.HeadId;
    DurableId sourceSessionId = initial.Records[0].SessionId;
    StrView oldToolNames[] = {SVL("read")};
    StrView newToolNames[] = {SVL("write")};
    SessionRecord entries[8] = {};
    entries[0].EntryId = DurableId_Generate();
    entries[0].Kind = SessionRecordKind::Model;
    entries[0].ModelEntry.ParentId = rootId;
    entries[0].ModelEntry.Provider = SVL("provider-old");
    entries[0].ModelEntry.Model = SVL("model-old");
    entries[1].EntryId = DurableId_Generate();
    entries[1].Kind = SessionRecordKind::Thinking;
    entries[1].ThinkingEntry.ParentId = entries[0].EntryId;
    entries[1].ThinkingEntry.Level = SVL("high");
    entries[2].EntryId = DurableId_Generate();
    entries[2].Kind = SessionRecordKind::Tools;
    entries[2].ToolsEntry.ParentId = entries[1].EntryId;
    entries[2].ToolsEntry.ToolNames = Array<StrView>{oldToolNames, 1, 1, nullptr};
    entries[3].EntryId = DurableId_Generate();
    entries[3].Kind = SessionRecordKind::Message;
    entries[3].MessageEntry.ParentId = entries[2].EntryId;
    entries[3].MessageEntry.Value = Message_User(&arena, SVL("old branch message"));
    entries[4].EntryId = DurableId_Generate();
    entries[4].Kind = SessionRecordKind::Model;
    entries[4].ModelEntry.ParentId = rootId;
    entries[4].ModelEntry.Provider = SVL("provider-new");
    entries[4].ModelEntry.Model = SVL("model-new");
    entries[5].EntryId = DurableId_Generate();
    entries[5].Kind = SessionRecordKind::Thinking;
    entries[5].ThinkingEntry.ParentId = entries[4].EntryId;
    entries[5].ThinkingEntry.Level = SVL("low");
    entries[6].EntryId = DurableId_Generate();
    entries[6].Kind = SessionRecordKind::Tools;
    entries[6].ToolsEntry.ParentId = entries[5].EntryId;
    entries[6].ToolsEntry.ToolNames = Array<StrView>{newToolNames, 1, 1, nullptr};
    entries[7].EntryId = DurableId_Generate();
    entries[7].Kind = SessionRecordKind::Message;
    entries[7].MessageEntry.ParentId = entries[6].EntryId;
    entries[7].MessageEntry.Value = Message_User(&arena, SVL("new branch message"));
    // Appended through the harness's own seam (not the raw log) so the
    // harness's maintained Records/Reduction stay in sync -- v2 never
    // reloads to pick up out-of-band writes.
    CHECK(SessionResult_IsOk(AgentHarness_Append(&harness, entries, 8)));
    CHECK(SessionResult_IsOk(AgentHarness_Navigate(&harness, entries[7].EntryId)));

    SessionReduction beforeNavigation = {};
    CHECK(SessionResult_IsOk(AgentHarness_Snapshot(&arena, &harness, &beforeNavigation)));
    CHECK_ALL(StrView_EqCStr(beforeNavigation.Provider, "provider-new"),
              StrView_EqCStr(beforeNavigation.Model, "model-new"),
              StrView_EqCStr(beforeNavigation.ThinkingLevel, "low"),
              beforeNavigation.ToolNames.Count == 1,
              StrView_EqCStr(beforeNavigation.ToolNames.Data[0], "write"));

    CHECK(SessionResult_IsOk(AgentHarness_Navigate(&harness, entries[3].EntryId)));
    SessionReduction afterNavigation = {};
    CHECK(SessionResult_IsOk(AgentHarness_Snapshot(&arena, &harness, &afterNavigation)));
    CHECK_ALL(DurableId_Eq(afterNavigation.HeadId, entries[3].EntryId),
              StrView_EqCStr(afterNavigation.Provider, "provider-old"),
              StrView_EqCStr(afterNavigation.Model, "model-old"),
              StrView_EqCStr(afterNavigation.ThinkingLevel, "high"),
              afterNavigation.ToolNames.Count == 1,
              StrView_EqCStr(afterNavigation.ToolNames.Data[0], "read"));

    CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("continue old branch"))));
    SessionReduction branched = {};
    CHECK(SessionResult_IsOk(AgentHarness_Snapshot(&arena, &harness, &branched)));
    Array<Message> context = {};
    CHECK(SessionResult_IsOk(SessionReducer_Context(&arena, &branched, branched.HeadId, &context)));
    CHECK_ALL(context.Count == 3, ContextTextEq(&context, 0, Role::User, "old branch message"),
              ContextTextEq(&context, 1, Role::User, "continue old branch"),
              ContextTextEq(&context, 2, Role::Assistant, "branched response"),
              StrView_EqCStr(branched.Provider, "provider-old"),
              StrView_EqCStr(branched.Model, "model-old"));
    CHECK(SessionResult_IsOk(AgentHarness_LabelBranch(&harness, SVL("old-branch"))));
    SessionReduction labeled = {};
    CHECK(SessionResult_IsOk(AgentHarness_Snapshot(&arena, &harness, &labeled)));
    Array<const SessionRecord*> labeledAncestry = {};
    CHECK(SessionResult_IsOk(
        SessionReducer_Ancestry(&labeled, &arena, labeled.HeadId, &labeledAncestry)));
    CHECK_ALL(labeledAncestry.Count > 0,
              labeledAncestry.Data[labeledAncestry.Count - 1]->Kind == SessionRecordKind::BranchLabel,
              StrView_EqCStr(
                  labeledAncestry.Data[labeledAncestry.Count - 1]->BranchLabelEntry.Label,
                  "old-branch"));

    SessionLog forkLog = {};
    SessionInfo forkInfo = {};
    CHECK(SessionResult_IsOk(AgentHarness_Fork(&arena, &harness, &repository, labeled.HeadId,
                                              DurableId{}, SVL("branched fork"), &forkLog,
                                              &forkInfo)));
    Array<SessionRecord> forkRecords = {};
    CHECK(SessionResult_IsOk(SessionLog_ReadAll(&forkLog, &arena, &forkRecords)));
    CHECK_ALL(forkRecords.Count == 8,
              DurableId_Eq(forkRecords.Data[0].SessionStarted.ForkedFromSessionId, sourceSessionId),
              DurableId_Eq(forkRecords.Data[0].SessionStarted.ForkedFromEntryId, labeled.HeadId));
    SessionLog_Close(&forkLog);

    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }
}

// ---------------------------------------------------------------------------
// Automatic compaction: fires from inside the turn loop when the context eats
// into the window's reserved headroom, and never as a nested agent lifecycle.
// ---------------------------------------------------------------------------
static AgentHarnessConfig AutoCompactConfig(ScriptProvider* script, Tool* tools, u32 toolCount,
                                            bool enabled, u32 keepRecentTokens = 10) {
  AgentHarnessConfig config = ScriptConfig(script, tools, toolCount);
  config.ModelInfo.ContextWindow = 1000;
  config.Compaction.Enabled = enabled;
  config.Compaction.ReserveTokens = 400;  // threshold at 600 prompt tokens
  config.Compaction.KeepRecentTokens = keepRecentTokens;
  return config;
}

static Usage PromptUsage(u32 input) {
  Usage use = {};
  use.Input = input;
  return use;
}

static void TestHarnessAutoCompaction() {
  u32 executions = 0;
  Tool echo[1] = {};
  echo[0].Name = SVL("echo");
  echo[0].Description = SVL("echo tool");
  echo[0].ParamsJsonSchema = SVL("{\"type\":\"object\"}");
  echo[0].Ctx = &executions;
  echo[0].Execute = EchoExec;

  ScriptToolCall calls[1] = {ScriptToolCall{SVL("echo"), SVL("call-1"), SVL("{\"x\":1}")}};

  // Over the threshold: the loop compacts once before the next provider call.
  {
    Arena arena = ArenaCreate((usize)32 << 20);
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));

    ScriptStep steps[3] = {};
    steps[0].Tools = calls;
    steps[0].ToolCount = 1;
    steps[0].Use = PromptUsage(700);
    steps[1].Text = SVL("compact summary");
    steps[1].Stop = StopReason::Stop;
    steps[2].Text = SVL("done");
    steps[2].Stop = StopReason::Stop;
    ScriptProvider script = {steps, 3, 0};

    AgentHarness harness = {};
    AgentHarnessConfig config = AutoCompactConfig(&script, echo, 1, true);
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));

    AgentHarnessSnapshot watchSnapshot = {};
    AgentHarnessWatch* watchPtr = ArenaPushStruct(&arena, AgentHarnessWatch);
    *watchPtr = AgentHarnessWatch{};
    AgentHarnessWatch& watch = *watchPtr;
    CHECK(SessionResult_IsOk(
        AgentHarness_Watch(&arena, &harness, 128, &watchSnapshot, &watch)));

    CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("hello"))));
    CHECK(script.Index == 3);  // turn, compaction summary, final turn

    Array<SessionRecord> records = {};
    CHECK(SessionResult_IsOk(SessionLog_ReadAll(&harness.Log, &arena, &records)));
    CHECK(CountKind(&records, SessionRecordKind::Compaction) == 1);

    // The compaction turn is reported at turn level only: a run must never
    // contain a nested Compaction-tagged agent lifecycle.
    u32 compactionTurnStarts = 0, compactionTurnEnds = 0, compactionAgentEvents = 0;
    u32 agentEnds = 0;
    while (agentEnds == 0) {
      AgentHarnessEvent event = {};
      CHECK(SessionResult_IsOk(AgentHarness_WatchNext(&arena, &watch, &event)));
      if (event.Kind == AgentHarnessEventKind::TransientTurnStart &&
          event.Turn.Kind == AgentTurnKind::Compaction)
        compactionTurnStarts++;
      else if (event.Kind == AgentHarnessEventKind::TransientTurnEnd &&
               event.Turn.Kind == AgentTurnKind::Compaction)
        compactionTurnEnds++;
      else if (event.Kind == AgentHarnessEventKind::TransientAgentStart &&
               event.Turn.Kind == AgentTurnKind::Compaction)
        compactionAgentEvents++;
      else if (event.Kind == AgentHarnessEventKind::TransientAgentEnd) {
        if (event.Turn.Kind == AgentTurnKind::Compaction)
          compactionAgentEvents++;
        else
          agentEnds++;
      }
    }
    CHECK_ALL(compactionTurnStarts == 1, compactionTurnEnds == 1,
              compactionAgentEvents == 0);

    CHECK(SessionResult_IsOk(AgentHarness_WatchClose(&watch)));
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Automatic compaction sizes against what the request will actually carry.
  // A small context override must not be destroyed by a compaction triggered
  // by the large durable branch it replaced.
  {
    Arena arena = ArenaCreate((usize)32 << 20);
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));

    // Turn 1 reports 700 prompt tokens, past the 600-token threshold, so the
    // branch alone would compact before turn 2.
    ScriptStep steps[3] = {};
    steps[0].Tools = calls;
    steps[0].ToolCount = 1;
    steps[0].Use = PromptUsage(700);
    steps[1].Text = SVL("done");
    steps[1].Stop = StopReason::Stop;
    steps[2].Text = SVL("must never run");
    steps[2].Stop = StopReason::Stop;
    ScriptProvider script = {steps, 3, 0};

    TurnHookState hooks = {};
    hooks.OverrideContext = true;
    // The trimmed context keeps turn 1's assistant message, which carries the
    // 700-token usage report. Sizing that anchor as if it still paid for the
    // discarded history would compact and destroy the override.
    hooks.KeepAssistantInOverride = true;
    AgentHarness harness = {};
    AgentHarnessConfig config = AutoCompactConfig(&script, echo, 1, true);
    config.Hooks.User = &hooks;
    config.Hooks.PrepareNextTurn = HookPrepareNextTurn;
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("override then compact"))));

    // The trimmed override is genuinely small, so no compaction turn is spent
    // and the override survives: exactly 2 provider calls, no Compaction entry.
    Array<SessionRecord> records = {};
    CHECK(SessionResult_IsOk(SessionLog_ReadAll(&harness.Log, &arena, &records)));
    CHECK_ALL(script.Index == 2, CountKind(&records, SessionRecordKind::Compaction) == 0,
              AllNonRootAreMessages(&records));
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Under the threshold: nothing compacts.
  {
    Arena arena = ArenaCreate((usize)32 << 20);
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));

    ScriptStep steps[2] = {};
    steps[0].Tools = calls;
    steps[0].ToolCount = 1;
    steps[0].Use = PromptUsage(100);
    steps[1].Text = SVL("done");
    steps[1].Stop = StopReason::Stop;
    ScriptProvider script = {steps, 2, 0};

    AgentHarness harness = {};
    AgentHarnessConfig config = AutoCompactConfig(&script, echo, 1, true);
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("hello"))));
    CHECK(script.Index == 2);

    Array<SessionRecord> records = {};
    CHECK(SessionResult_IsOk(SessionLog_ReadAll(&harness.Log, &arena, &records)));
    CHECK(CountKind(&records, SessionRecordKind::Compaction) == 0);

    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // A disabled policy never fires, however much context accumulates.
  {
    Arena arena = ArenaCreate((usize)32 << 20);
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));

    ScriptStep steps[2] = {};
    steps[0].Tools = calls;
    steps[0].ToolCount = 1;
    steps[0].Use = PromptUsage(700);
    steps[1].Text = SVL("done");
    steps[1].Stop = StopReason::Stop;
    ScriptProvider script = {steps, 2, 0};

    AgentHarness harness = {};
    AgentHarnessConfig config = AutoCompactConfig(&script, echo, 1, false);
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("hello"))));
    CHECK(script.Index == 2);

    Array<SessionRecord> records = {};
    CHECK(SessionResult_IsOk(SessionLog_ReadAll(&harness.Log, &arena, &records)));
    CHECK(CountKind(&records, SessionRecordKind::Compaction) == 0);

    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // With a budget that spans two user turns, the tail is retained: the summary
  // replaces only the older prefix and the recent turns survive verbatim.
  {
    Arena arena = ArenaCreate((usize)32 << 20);
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));

    ScriptStep steps[4] = {};
    steps[0].Text = SVL("ok");
    steps[0].Stop = StopReason::Stop;
    steps[0].Use = PromptUsage(100);  // first run stays under the threshold
    steps[1].Tools = calls;
    steps[1].ToolCount = 1;
    steps[1].Use = PromptUsage(700);  // second run crosses it
    steps[2].Text = SVL("compact summary");
    steps[2].Stop = StopReason::Stop;
    steps[3].Text = SVL("done");
    steps[3].Stop = StopReason::Stop;
    ScriptProvider script = {steps, 4, 0};

    AgentHarness harness = {};
    AgentHarnessConfig config = AutoCompactConfig(&script, echo, 1, true, 20);
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));

    CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("hello"))));
    CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("second"))));
    CHECK(script.Index == 4);

    Array<SessionRecord> records = {};
    CHECK(SessionResult_IsOk(SessionLog_ReadAll(&harness.Log, &arena, &records)));
    CHECK(CountKind(&records, SessionRecordKind::Compaction) == 1);

    SessionReduction snapshot = {};
    CHECK(SessionResult_IsOk(AgentHarness_Snapshot(&arena, &harness, &snapshot)));
    Array<Message> context = {};
    CHECK(SessionResult_IsOk(
        SessionReducer_Context(&arena, &snapshot, snapshot.HeadId, &context)));

    // The summary leads, the first user turn is gone, and the second survives.
    bool sawSummary = false, sawHello = false, sawSecond = false;
    for (u32 i = 0; i < context.Count; i++) {
      const Message* message = &context.Data[i];
      for (u32 k = 0; k < message->Content.Count; k++) {
        StrView text = message->Content.Data[k].Text;
        if (StrView_Contains(text, "compact summary")) sawSummary = true;
        if (StrView_EqCStr(text, "hello")) sawHello = true;
        if (StrView_EqCStr(text, "second")) sawSecond = true;
      }
    }
    CHECK_ALL(sawSummary, !sawHello, sawSecond, context.Count > 1);

    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // A failing compaction turn aborts the run with a message that names
  // compaction, rather than letting the provider reject an oversized request.
  {
    Arena arena = ArenaCreate((usize)32 << 20);
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));

    ScriptStep steps[2] = {};
    steps[0].Tools = calls;
    steps[0].ToolCount = 1;
    steps[0].Use = PromptUsage(700);
    steps[1].Error = SVL("summary provider exploded");
    ScriptProvider script = {steps, 2, 0};

    AgentHarness harness = {};
    AgentHarnessConfig config = AutoCompactConfig(&script, echo, 1, true);
    config.Retry.Enabled = false;  // fail on the first compaction attempt
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));

    SessionResult run = AgentHarness_Run(&harness, SVL("hello"));
    CHECK(run.Status == SessionStatus::ProviderError);
    CHECK(StrView_Contains(run.Message, "automatic compaction failed"));

    Array<SessionRecord> records = {};
    CHECK(SessionResult_IsOk(SessionLog_ReadAll(&harness.Log, &arena, &records)));
    CHECK(CountKind(&records, SessionRecordKind::Compaction) == 0);

    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }
}

// Closing a watch stops further publication but must not discard what was
// already published: a consumer that shuts down right after a run still needs
// that run's trailing turn_end/agent_end.
static void TestHarnessWatchDrainsAfterClose() {
  Arena arena = ArenaCreate((usize)32 << 20);
  MemoryRepository memory = {};
  MemoryRepository_Init(&memory);
  SessionRepository repository = MemoryRepository_AsRepository(&memory);
  SessionLog log = {};
  CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));

  ScriptStep step = {};
  step.Text = SVL("done");
  step.Stop = StopReason::Stop;
  ScriptProvider script = {&step, 1, 0};

  AgentHarness harness = {};
  AgentHarnessConfig config = ScriptConfig(&script, nullptr, 0);
  CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));

  AgentHarnessSnapshot snapshot = {};
  AgentHarnessWatch* watchPtr = ArenaPushStruct(&arena, AgentHarnessWatch);
  *watchPtr = AgentHarnessWatch{};
  AgentHarnessWatch& watch = *watchPtr;
  CHECK(SessionResult_IsOk(
      AgentHarness_Watch(&arena, &harness, 64, &snapshot, &watch)));

  CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("hello"))));

  // Request the close before reading anything, so every event of the run is
  // still sitting in the ring buffer.
  CHECK(SessionResult_IsOk(AgentHarness_WatchRequestClose(&watch)));

  u32 drained = 0;
  bool sawAgentEnd = false;
  SessionResult next = SessionResult_Ok();
  for (;;) {
    AgentHarnessEvent event = {};
    next = AgentHarness_WatchNext(&arena, &watch, &event);
    if (!SessionResult_IsOk(next)) break;
    drained++;
    if (event.Kind == AgentHarnessEventKind::TransientAgentEnd) sawAgentEnd = true;
  }
  CHECK_ALL(drained > 0, sawAgentEnd, next.Status == SessionStatus::InvalidState);

  CHECK(SessionResult_IsOk(AgentHarness_WatchClose(&watch)));
  CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
  repository.Close(repository.Ctx);
  ArenaRelease(&arena);
}

// Ring-buffer behaviour under a consumer that cannot keep up. Driving the
// publish seams directly keeps this focused on the watcher rather than on a
// provider script's timing.
void TestHarnessWatchOverflow() {
  Arena arena = ArenaCreate();
  MemoryRepository memory = {};
  MemoryRepository_Init(&memory);
  SessionRepository repository = MemoryRepository_AsRepository(&memory);
  SessionLog log = {};
  CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));

  ScriptStep step = {};
  step.Text = SVL("overflow");
  step.Stop = StopReason::Stop;
  ScriptProvider script = {&step, 1, 0};
  AgentHarnessConfig config = ScriptConfig(&script, nullptr, 0);
  AgentHarness harness = {};
  CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));

  // Adjacent text deltas on the same content block collapse into one queued
  // event, so a burst far larger than the ring still fits.
  {
    AgentHarnessSnapshot snapshot = {};
    AgentHarnessWatch watch = {};
    CHECK(SessionResult_IsOk(
        AgentHarness_Watch(&arena, &harness, 8, &snapshot, &watch)));

    for (u32 i = 0; i < 200; i++) {
      StreamEvent delta = {};
      delta.Kind = StreamEventKind::TextDelta;
      delta.ContentIndex = 0;
      delta.Delta = SVL("x");
      AgentHarness_PublishStream(&harness, &delta);
    }
    CHECK_ALL(watch.Count == 1, !watch.Lagged, watch.DroppedTransient == 0);

    AgentHarnessEvent event = {};
    CHECK(SessionResult_IsOk(AgentHarness_WatchNext(&arena, &watch, &event)));
    CHECK_ALL(event.Kind == AgentHarnessEventKind::TransientStream,
              event.Stream.Delta.Length == 200);
    // Draining the ring returns its backing memory rather than growing forever.
    CHECK_ALL(watch.Count == 0, watch.Mem.Used == 0);
    CHECK(SessionResult_IsOk(AgentHarness_WatchClose(&watch)));
  }

  // A different content index cannot merge, so the ring genuinely overflows.
  // The watcher keeps running and reports the gap instead of dying.
  {
    AgentHarnessSnapshot snapshot = {};
    AgentHarnessWatch watch = {};
    CHECK(SessionResult_IsOk(
        AgentHarness_Watch(&arena, &harness, 8, &snapshot, &watch)));

    for (u32 i = 0; i < 20; i++) {
      StreamEvent delta = {};
      delta.Kind = StreamEventKind::TextDelta;
      delta.ContentIndex = i;  // never adjacent-mergeable
      delta.Delta = SVL("y");
      AgentHarness_PublishStream(&harness, &delta);
    }
    CHECK_ALL(!watch.Lagged, watch.Count == 8, watch.DroppedTransient == 12);

    // The gap is reported once, at the head, and then the stream resumes with
    // the oldest surviving event.
    AgentHarnessEvent event = {};
    CHECK(SessionResult_IsOk(AgentHarness_WatchNext(&arena, &watch, &event)));
    CHECK_ALL(event.Kind == AgentHarnessEventKind::TransientEventsDropped,
              event.Dropped.Count == 12);
    CHECK(SessionResult_IsOk(AgentHarness_WatchNext(&arena, &watch, &event)));
    CHECK_ALL(event.Kind == AgentHarnessEventKind::TransientStream,
              event.Stream.ContentIndex == 12);
    CHECK(SessionResult_IsOk(AgentHarness_WatchClose(&watch)));
  }

  // Durable records are never sacrificed to make room: losing one would leave
  // the consumer's transcript short an entry.
  {
    AgentHarnessSnapshot snapshot = {};
    AgentHarnessWatch watch = {};
    CHECK(SessionResult_IsOk(
        AgentHarness_Watch(&arena, &harness, 4, &snapshot, &watch)));

    CHECK(SessionResult_IsOk(AgentHarness_QueueNextTurn(&harness, SVL("queued"))));
    CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("hello"))));

    u32 durable = 0;
    u32 dropped = 0;
    for (;;) {
      AgentHarnessEvent event = {};
      SessionResult next = AgentHarness_WatchNext(&arena, &watch, &event);
      if (!SessionResult_IsOk(next)) break;
      if (event.Kind == AgentHarnessEventKind::DurableRecord) durable++;
      if (event.Kind == AgentHarnessEventKind::TransientEventsDropped)
        dropped += event.Dropped.Count;
      if (event.Kind == AgentHarnessEventKind::TransientAgentEnd) break;
    }
    // A four-slot ring cannot hold a whole run, so events were certainly
    // discarded -- yet every durable record still arrived.
    SessionReduction reduction = AgentHarness_ReduceSnapshot(&harness);
    CHECK_ALL(!watch.Lagged, dropped > 0, durable == reduction.RecordCount - 1);
    CHECK(SessionResult_IsOk(AgentHarness_WatchClose(&watch)));
  }

  // A consumer that stops draining entirely must not be able to grow the ring's
  // arena without bound. Coalescing used to reallocate at the exact merged size
  // on every delta, so a tail built from N deltas cost O(N^2) arena: 20k
  // one-byte deltas consumed ~200 MB, and a longer stall reached the arena
  // reservation and trapped the process instead of degrading.
  {
    AgentHarnessSnapshot snapshot = {};
    AgentHarnessWatch watch = {};
    CHECK(SessionResult_IsOk(
        AgentHarness_Watch(&arena, &harness, 8, &snapshot, &watch)));

    for (u32 i = 0; i < 20000; i++) {
      StreamEvent delta = {};
      delta.Kind = StreamEventKind::TextDelta;
      delta.ContentIndex = 0;
      delta.Delta = SVL("z");
      AgentHarness_PublishStream(&harness, &delta);
    }
    // Geometric growth keeps the total a small multiple of the text, and the
    // coalesce limit caps each tail, so this stays in the low hundreds of KB.
    CHECK_ALL(!watch.Lagged, watch.Mem.Used < ((usize)4 << 20));

    // The merged text is still intact.
    AgentHarnessEvent event = {};
    CHECK(SessionResult_IsOk(AgentHarness_WatchNext(&arena, &watch, &event)));
    CHECK(event.Kind == AgentHarnessEventKind::TransientStream);
    bool allZ = true;
    for (u32 i = 0; i < event.Stream.Delta.Length; i++)
      if (event.Stream.Delta.Data[i] != 'z') allZ = false;
    CHECK(allZ);
    CHECK(SessionResult_IsOk(AgentHarness_WatchClose(&watch)));
  }

  CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
  ArenaRelease(&arena);
}

void TestHarness() {
  TestHarnessReplayAndIncremental();
  TestHarnessLifecycle();
  TestHarnessWatchers();
  TestHarnessWatchOverflow();
  TestHarnessWatchDrainsAfterClose();
  TestHarnessToolsAndHooks();
  TestHarnessPerTurnHooks();
  TestHarnessCancellationAndBranches();
  TestHarnessAutoCompaction();
}
