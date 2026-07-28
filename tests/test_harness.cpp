#include "agent/harness.h"
#include "base/arena.h"
#include "base/thread.h"
#include "check.h"
#include "llm/faux.h"
#include "session/memory_repository.h"

namespace {

SessionResult CreateSession(SessionRepository* repository, Arena* arena,
                            SessionLog* out) {
  SessionCreateOptions options = {};
  options.ProjectKey = SVL("harness-tests");
  options.WorkingDirectory = SVL("C:\\work\\clank");
  options.Label = SVL("harness");
  SessionInfo info = {};
  return repository->Create(repository->Ctx, arena, &options, out, &info);
}

bool EchoExec(void* context, Arena* arena, StrView id, StrView argsJson,
              volatile i32* abort, ToolResult* out, StrBuf* error) {
  (void)context;
  (void)id;
  (void)abort;
  (void)error;
  StrBuf text = StrBuf_Make(arena, 32);
  StrBuf_AppendCStr(&text, "echo:");
  StrBuf_Append(&text, argsJson);
  out->Content = Array_Make<Content>(arena, 1);
  Array_Push(&out->Content, Content_Text(StrBuf_View(&text)));
  return true;
}

bool SteerExec(void* context, Arena* arena, StrView id, StrView argsJson,
               volatile i32* abort, ToolResult* out, StrBuf* error) {
  (void)id;
  (void)argsJson;
  (void)abort;
  AgentHarness* harness = (AgentHarness*)context;
  SessionResult queued =
      AgentHarness_QueueSteering(harness, SVL("steered durably"));
  if (!SessionResult_IsOk(queued)) {
    StrBuf_Append(error, queued.Message);
    return false;
  }
  out->Content = Array_Make<Content>(arena, 1);
  Array_Push(&out->Content, Content_Text(SVL("queued")));
  return true;
}

bool CountReplayExec(void* context, Arena* arena, StrView id, StrView argsJson,
                     volatile i32* abort, ToolResult* out, StrBuf* error) {
  (void)id;
  (void)argsJson;
  (void)abort;
  (void)error;
  (*(u32*)context)++;
  out->Content = Array_Make<Content>(arena, 1);
  Array_Push(&out->Content, Content_Text(SVL("replayed")));
  return true;
}

struct HarnessHookState {
  u32 BeforeRunCount;
  u32 BeforeContextCount;
  u32 BeforeProviderCount;
  u32 AfterProviderCount;
  u32 BeforeToolCount;
  u32 CompletionCount;
  u32 FirstContextCount;
  u32 LastContextCount;
};

SessionResult HookBeforeRun(void* context, Arena* arena, Message* prompt) {
  HarnessHookState* state = (HarnessHookState*)context;
  state->BeforeRunCount++;
  prompt->Content.Data[0].Text =
      StrView_Dup(arena, SVL("hooked durable prompt"));
  return SessionResult_Ok();
}

SessionResult HookBeforeContext(void* context, Arena* arena,
                                OperationKind operation, DurableId headId,
                                Array<Message>* messages) {
  (void)operation;
  (void)headId;
  HarnessHookState* state = (HarnessHookState*)context;
  state->BeforeContextCount++;
  Array_Push(messages, Message_User(arena, SVL("request-only hook context")));
  if (state->BeforeContextCount == 1)
    state->FirstContextCount = messages->Count;
  state->LastContextCount = messages->Count;
  return SessionResult_Ok();
}

SessionResult HookBeforeProvider(void* context, Arena* arena,
                                 OperationKind operation, u32 attemptIndex,
                                 AgentHarnessProviderInput* input) {
  (void)operation;
  (void)attemptIndex;
  HarnessHookState* state = (HarnessHookState*)context;
  state->BeforeProviderCount++;
  input->ModelInfo.Provider = StrView_Dup(arena, SVL("hook-provider"));
  input->ModelInfo.Id = StrView_Dup(arena, SVL("hook-model"));
  input->Options.MaxTokens = 321;
  return SessionResult_Ok();
}

SessionResult HookAfterProvider(void* context, Arena* arena,
                                OperationKind operation, u32 attemptIndex,
                                Message* assistant) {
  (void)arena;
  (void)operation;
  (void)attemptIndex;
  (void)assistant;
  ((HarnessHookState*)context)->AfterProviderCount++;
  return SessionResult_Ok();
}

SessionResult HookBeforeTool(void* context, Arena* arena, StrView toolName,
                             StrView* argumentsJson) {
  (void)toolName;
  HarnessHookState* state = (HarnessHookState*)context;
  state->BeforeToolCount++;
  *argumentsJson = StrView_Dup(arena, SVL("{\"hooked\":true}"));
  return SessionResult_Ok();
}

SessionResult HookBeforeComplete(void* context, Arena* arena,
                                 OperationKind operation,
                                 OperationOutcome outcome, StrView error) {
  (void)arena;
  (void)operation;
  (void)outcome;
  (void)error;
  ((HarnessHookState*)context)->CompletionCount++;
  return SessionResult_Ok();
}

SessionResult FailBeforeTool(void* context, Arena* arena, StrView toolName,
                             StrView* argumentsJson) {
  (void)arena;
  (void)toolName;
  (void)argumentsJson;
  (*(u32*)context)++;
  return SessionResult_Error(SessionStatus::InvalidArgument,
                             SVL("tool hook failed"));
}

AgentHarnessConfig FauxConfig(Faux* faux, Tool* tools, u32 toolCount) {
  AgentHarnessConfig config = {};
  config.SystemPrompt = SVL("durable system");
  config.ModelInfo.Id = SVL("faux-model");
  config.ModelInfo.Provider = SVL("faux");
  config.Tools = tools;
  config.ToolCount = toolCount;
  config.ProviderImpl = Faux_Provider(faux);
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

bool ContextTextEq(const Array<Message>* messages, u32 index, Role role,
                   const char* text) {
  if (index >= messages->Count) return false;
  const Message* message = &messages->Data[index];
  return message->Kind == role && message->Content.Count > 0 &&
         StrView_EqCStr(message->Content.Data[0].Text, text);
}

struct SuspendedToolTrace {
  DurableId OperationId;
  DurableId ToolExecutionId;
};

struct CancelProviderState {
  volatile i32 Started;
};

bool CancelProviderStream(void* context, Arena* arena, const Model* model,
                          const LlmContext* llm, const StreamOptions* options,
                          StreamSink sink, Message* out) {
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

struct HarnessRunThread {
  AgentHarness* Harness;
  SessionResult Result;
};

void RunHarnessThread(void* context) {
  HarnessRunThread* run = (HarnessRunThread*)context;
  run->Result = AgentHarness_Run(run->Harness, SVL("cancel me"));
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
  wait->Result =
      AgentHarness_WatchNext(&arena, wait->Watch, &event);
  ArenaRelease(&arena);
}

SessionResult AppendSuspendedProvider(SessionLog* log, Arena* arena,
                                      DurableId* outOperationId) {
  Array<SessionRecord> root = {};
  SessionResult read = SessionLog_ReadAll(log, arena, &root);
  if (!SessionResult_IsOk(read)) return read;
  DurableId operationId = DurableId_Generate();
  DurableId promptId = DurableId_Generate();
  DurableId attemptId = DurableId_Generate();

  SessionRecord records[3] = {};
  records[0].EntryId = DurableId_Generate();
  records[0].Kind = SessionRecordKind::OperationStarted;
  records[0].OperationStarted.OperationId = operationId;
  records[0].OperationStarted.Operation = OperationKind::Run;
  records[0].OperationStarted.StartingHeadId = root.Data[0].EntryId;
  records[0].OperationStarted.HasAcceptedMessage = true;
  records[0].OperationStarted.AcceptedMessage =
      Message_User(arena, SVL("resume provider"));
  records[0].OperationStarted.ProvisionedEntryId = promptId;

  records[1].EntryId = promptId;
  records[1].Kind = SessionRecordKind::Message;
  records[1].MessageEntry.ParentId = root.Data[0].EntryId;
  records[1].MessageEntry.Value = records[0].OperationStarted.AcceptedMessage;

  records[2].EntryId = DurableId_Generate();
  records[2].Kind = SessionRecordKind::GenerationStarted;
  records[2].GenerationStarted.OperationId = operationId;
  records[2].GenerationStarted.AttemptId = attemptId;
  records[2].GenerationStarted.Provider = SVL("faux");
  records[2].GenerationStarted.Model = SVL("faux-model");
  records[2].GenerationStarted.EffectiveRequestJson = SVL("{}");
  records[2].GenerationStarted.MaxTokens = 0;
  records[2].GenerationStarted.ProvisionedAssistantEntryId =
      DurableId_Generate();
  SessionResult appended = SessionLog_AppendBatch(log, records, 3);
  if (SessionResult_IsOk(appended)) *outOperationId = operationId;
  return appended;
}

SessionResult AppendSuspendedTool(SessionLog* log, Arena* arena,
                                  ToolReplayPolicy replay,
                                  SuspendedToolTrace* out) {
  Array<SessionRecord> root = {};
  SessionResult read = SessionLog_ReadAll(log, arena, &root);
  if (!SessionResult_IsOk(read)) return read;
  DurableId operationId = DurableId_Generate();
  DurableId promptId = DurableId_Generate();
  DurableId attemptId = DurableId_Generate();
  DurableId assistantId = DurableId_Generate();
  DurableId executionId = DurableId_Generate();

  Message assistant = Message_AssistantEmpty(arena);
  assistant.Stop = StopReason::ToolUse;
  assistant.Provider = SVL("faux");
  assistant.Model = SVL("faux-model");
  Array_Push(&assistant.Content,
             Content_ToolCall(SVL("call-recover"), SVL("recover"), SVL("{}")));

  SessionRecord records[6] = {};
  records[0].EntryId = DurableId_Generate();
  records[0].Kind = SessionRecordKind::OperationStarted;
  records[0].OperationStarted.OperationId = operationId;
  records[0].OperationStarted.Operation = OperationKind::Run;
  records[0].OperationStarted.StartingHeadId = root.Data[0].EntryId;
  records[0].OperationStarted.HasAcceptedMessage = true;
  records[0].OperationStarted.AcceptedMessage =
      Message_User(arena, SVL("resume tool"));
  records[0].OperationStarted.ProvisionedEntryId = promptId;

  records[1].EntryId = promptId;
  records[1].Kind = SessionRecordKind::Message;
  records[1].MessageEntry.ParentId = root.Data[0].EntryId;
  records[1].MessageEntry.Value = records[0].OperationStarted.AcceptedMessage;

  records[2].EntryId = DurableId_Generate();
  records[2].Kind = SessionRecordKind::GenerationStarted;
  records[2].GenerationStarted.OperationId = operationId;
  records[2].GenerationStarted.AttemptId = attemptId;
  records[2].GenerationStarted.Provider = SVL("faux");
  records[2].GenerationStarted.Model = SVL("faux-model");
  records[2].GenerationStarted.EffectiveRequestJson = SVL("{}");
  records[2].GenerationStarted.MaxTokens = 0;
  records[2].GenerationStarted.ProvisionedAssistantEntryId = assistantId;

  records[3].EntryId = assistantId;
  records[3].Kind = SessionRecordKind::Message;
  records[3].MessageEntry.ParentId = promptId;
  records[3].MessageEntry.Value = assistant;

  records[4].EntryId = DurableId_Generate();
  records[4].Kind = SessionRecordKind::GenerationFinished;
  records[4].GenerationFinished.OperationId = operationId;
  records[4].GenerationFinished.AttemptId = attemptId;
  records[4].GenerationFinished.Outcome = GenerationOutcome::Succeeded;
  records[4].GenerationFinished.MessageEntryId = assistantId;

  records[5].EntryId = DurableId_Generate();
  records[5].Kind = SessionRecordKind::ToolStarted;
  records[5].ToolStarted.OperationId = operationId;
  records[5].ToolStarted.ToolExecutionId = executionId;
  records[5].ToolStarted.ToolCallId = SVL("call-recover");
  records[5].ToolStarted.ToolName = SVL("recover");
  records[5].ToolStarted.EffectiveArgumentsJson = SVL("{}");
  records[5].ToolStarted.ProvisionedResultEntryId = DurableId_Generate();
  records[5].ToolStarted.ReplayPolicy = replay;
  SessionResult appended = SessionLog_AppendBatch(log, records, 6);
  if (SessionResult_IsOk(appended)) {
    out->OperationId = operationId;
    out->ToolExecutionId = executionId;
  }
  return appended;
}

SessionResult AppendSuspendedCompaction(SessionLog* log, Arena* arena) {
  Array<SessionRecord> root = {};
  SessionResult read = SessionLog_ReadAll(log, arena, &root);
  if (!SessionResult_IsOk(read)) return read;
  DurableId userId = DurableId_Generate();
  DurableId operationId = DurableId_Generate();
  DurableId attemptId = DurableId_Generate();
  DurableId compactionId = DurableId_Generate();

  SessionRecord records[3] = {};
  records[0].EntryId = userId;
  records[0].Kind = SessionRecordKind::Message;
  records[0].MessageEntry.ParentId = root.Data[0].EntryId;
  records[0].MessageEntry.Value =
      Message_User(arena, SVL("conversation before interrupted compaction"));

  records[1].EntryId = DurableId_Generate();
  records[1].Kind = SessionRecordKind::OperationStarted;
  records[1].OperationStarted.OperationId = operationId;
  records[1].OperationStarted.Operation = OperationKind::Compaction;
  records[1].OperationStarted.StartingHeadId = userId;
  records[1].OperationStarted.ProvisionedEntryId = compactionId;

  records[2].EntryId = DurableId_Generate();
  records[2].Kind = SessionRecordKind::GenerationStarted;
  records[2].GenerationStarted.OperationId = operationId;
  records[2].GenerationStarted.AttemptId = attemptId;
  records[2].GenerationStarted.Provider = SVL("faux");
  records[2].GenerationStarted.Model = SVL("faux-model");
  records[2].GenerationStarted.EffectiveRequestJson = SVL("{}");
  records[2].GenerationStarted.ProvisionedAssistantEntryId = compactionId;
  return SessionLog_AppendBatch(log, records, 3);
}

SessionResult AppendTornDeferredCheckpoint(SessionLog* log, Arena* arena) {
  Array<SessionRecord> root = {};
  SessionResult read = SessionLog_ReadAll(log, arena, &root);
  if (!SessionResult_IsOk(read)) return read;
  DurableId operationId = DurableId_Generate();
  DurableId promptId = DurableId_Generate();
  DurableId itemId = DurableId_Generate();
  DurableId deferredId = DurableId_Generate();
  DurableId queuedMessageId = DurableId_Generate();
  Message prompt = Message_User(arena, SVL("prompt before queued steering"));
  Message queued = Message_User(arena, SVL("queued once"));

  SessionRecord records[6] = {};
  records[0].EntryId = DurableId_Generate();
  records[0].Kind = SessionRecordKind::OperationStarted;
  records[0].OperationStarted.OperationId = operationId;
  records[0].OperationStarted.Operation = OperationKind::Run;
  records[0].OperationStarted.StartingHeadId = root.Data[0].EntryId;
  records[0].OperationStarted.HasAcceptedMessage = true;
  records[0].OperationStarted.AcceptedMessage = prompt;
  records[0].OperationStarted.ProvisionedEntryId = promptId;

  records[1].EntryId = promptId;
  records[1].Kind = SessionRecordKind::Message;
  records[1].MessageEntry.ParentId = root.Data[0].EntryId;
  records[1].MessageEntry.Value = prompt;

  records[2].EntryId = DurableId_Generate();
  records[2].Kind = SessionRecordKind::QueueItemAccepted;
  records[2].QueueItemAccepted.OperationId = operationId;
  records[2].QueueItemAccepted.ItemId = itemId;
  records[2].QueueItemAccepted.Queue = QueueKind::Steering;
  records[2].QueueItemAccepted.Value = queued;

  records[3].EntryId = DurableId_Generate();
  records[3].Kind = SessionRecordKind::DeferredWrite;
  records[3].DeferredWrite.OperationId = operationId;
  records[3].DeferredWrite.DeferredId = deferredId;
  records[3].DeferredWrite.QueueItemId = itemId;
  records[3].DeferredWrite.ProvisionedEntryId = queuedMessageId;
  records[3].DeferredWrite.Value = queued;

  records[4].EntryId = queuedMessageId;
  records[4].Kind = SessionRecordKind::Message;
  records[4].MessageEntry.ParentId = promptId;
  records[4].MessageEntry.Value = queued;

  records[5].EntryId = DurableId_Generate();
  records[5].Kind = SessionRecordKind::DeferredWriteCommitted;
  records[5].DeferredWriteCommitted.OperationId = operationId;
  records[5].DeferredWriteCommitted.DeferredId = deferredId;
  records[5].DeferredWriteCommitted.MessageEntryId = queuedMessageId;
  return SessionLog_AppendBatch(log, records, 6);
}

}  // namespace

void TestHarness() {
  // A complete tool run records every intent/result transition while exposing
  // only session messages to context assembly.
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
    echo.ReplayPolicy = ToolReplayPolicy::ReplaySafe;
    echo.Execute = EchoExec;
    FauxStep steps[2] = {};
    steps[0].ToolName = SVL("echo");
    steps[0].ToolId = SVL("call-1");
    steps[0].ToolArgsJson = SVL("{\"x\":1}");
    steps[1].Text = SVL("done");
    steps[1].Stop = StopReason::Stop;
    Faux faux = {};
    faux.Steps = steps;
    faux.Count = 2;

    AgentHarness harness = {};
    AgentHarnessConfig config = FauxConfig(&faux, &echo, 1);
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("hello"))));

    SessionReduction snapshot = {};
    CHECK(SessionResult_IsOk(
        AgentHarness_Snapshot(&arena, &harness, &snapshot)));
    Array<Message> context = {};
    CHECK(SessionResult_IsOk(SessionReducer_Context(
        &arena, &snapshot, snapshot.HeadId, &context)));
    CHECK_ALL(context.Count == 4, ContextTextEq(&context, 0, Role::User, "hello"),
              context.Data[1].Kind == Role::Assistant,
              ContextTextEq(&context, 2, Role::ToolResult, "echo:{\"x\":1}"),
              ContextTextEq(&context, 3, Role::Assistant, "done"),
              !snapshot.HasActiveOperation);

    Array<SessionRecord> records = {};
    CHECK(SessionResult_IsOk(
        SessionLog_ReadAll(&harness.Log, &arena, &records)));
    CHECK_ALL(CountKind(&records, SessionRecordKind::OperationStarted) == 1,
              CountKind(&records, SessionRecordKind::GenerationStarted) == 2,
              CountKind(&records, SessionRecordKind::ToolStarted) == 1,
              CountKind(&records, SessionRecordKind::ToolFinished) == 1,
              CountKind(&records, SessionRecordKind::OperationFinished) == 1);
    CHECK(records.Data[1].Kind == SessionRecordKind::OperationStarted);

    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Watch atomically returns a snapshot cursor and then delivers every durable
  // transition plus transient provider stream events in publication order.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    FauxStep step = {};
    step.Text = SVL("watched response");
    step.Stop = StopReason::Stop;
    Faux faux = {};
    faux.Steps = &step;
    faux.Count = 1;
    AgentHarness harness = {};
    AgentHarnessConfig config = FauxConfig(&faux, nullptr, 0);
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));

    AgentHarnessSnapshot initial = {};
    AgentHarnessWatch watch = {};
    CHECK(SessionResult_IsOk(
        AgentHarness_Watch(&arena, &harness, 16, &initial, &watch)));
    CHECK_ALL(initial.Cursor == 1, initial.Transcript.Count == 0,
              !initial.Running, !initial.Suspended, !initial.Faulted);
    CHECK(SessionResult_IsOk(
        AgentHarness_Run(&harness, SVL("watched prompt"))));

    u32 durableCount = 0;
    u32 transientCount = 0;
    bool sawDelta = false;
    u64 durableCursor = initial.Cursor;
    for (u32 i = 0; i < 11; i++) {
      AgentHarnessEvent event = {};
      CHECK(SessionResult_IsOk(
          AgentHarness_WatchNext(&arena, &watch, &event)));
      if (event.Kind == AgentHarnessEventKind::DurableRecord) {
        durableCount++;
        durableCursor++;
        CHECK_ALL(event.Cursor == durableCursor,
                  event.Record.Seq == event.Cursor);
      } else {
        transientCount++;
        if (event.Stream.Kind == StreamEventKind::TextDelta) {
          sawDelta = true;
          CHECK(StrView_EqCStr(event.Stream.Delta, "watched response"));
        }
      }
    }
    CHECK_ALL(durableCount == 6, transientCount == 5, sawDelta,
              durableCursor == 7);

    AgentHarnessSnapshot complete = {};
    CHECK(SessionResult_IsOk(
        AgentHarness_SnapshotFull(&arena, &harness, &complete)));
    CHECK_ALL(complete.Cursor == 7, complete.Transcript.Count == 2,
              ContextTextEq(&complete.Transcript, 0, Role::User,
                            "watched prompt"),
              ContextTextEq(&complete.Transcript, 1, Role::Assistant,
                            "watched response"));
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
    FauxStep step = {};
    step.Text = SVL("overflow");
    step.Stop = StopReason::Stop;
    Faux faux = {};
    faux.Steps = &step;
    faux.Count = 1;
    AgentHarness harness = {};
    AgentHarnessConfig config = FauxConfig(&faux, nullptr, 0);
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    AgentHarnessSnapshot snapshot = {};
    AgentHarnessWatch watch = {};
    CHECK(SessionResult_IsOk(
        AgentHarness_Watch(&arena, &harness, 2, &snapshot, &watch)));
    CHECK(SessionResult_IsOk(
        AgentHarness_Run(&harness, SVL("overflow prompt"))));
    AgentHarnessEvent event = {};
    CHECK(AgentHarness_WatchNext(&arena, &watch, &event).Status ==
          SessionStatus::WatcherLagged);
    CHECK(SessionResult_IsOk(AgentHarness_WatchClose(&watch)));
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Concurrent control-plane appends remain ordered with the driver append
  // stream, so a watcher sees every committed sequence exactly once.
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
    AgentHarnessWatch watch = {};
    CHECK(SessionResult_IsOk(
        AgentHarness_Watch(&arena, &harness, 32, &snapshot, &watch)));

    HarnessRunThread run = {};
    run.Harness = &harness;
    Thread thread = {};
    CHECK(Thread_Start(&thread, RunHarnessThread, &run));
    u32 waited = 0;
    while (!providerState.Started && waited < 2000) {
      Sleep_Ms(1);
      waited++;
    }
    CHECK(providerState.Started != 0);
    CHECK(SessionResult_IsOk(
        AgentHarness_QueueSteering(&harness, SVL("queued concurrently"))));
    CHECK(SessionResult_IsOk(AgentHarness_RequestCancel(&harness)));
    Thread_Join(&thread);
    CHECK(run.Result.Status == SessionStatus::Cancelled);

    Array<SessionRecord> records = {};
    CHECK(SessionResult_IsOk(
        SessionLog_ReadAll(&harness.Log, &arena, &records)));
    u32 expected = records.Count - (u32)snapshot.Cursor;
    u64 cursor = snapshot.Cursor;
    for (u32 i = 0; i < expected; i++) {
      AgentHarnessEvent event = {};
      CHECK(SessionResult_IsOk(
          AgentHarness_WatchNext(&arena, &watch, &event)));
      CHECK(event.Kind == AgentHarnessEventKind::DurableRecord);
      cursor++;
      CHECK(event.Cursor == cursor);
    }
    CHECK(cursor == records.Data[records.Count - 1].Seq);
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
    Faux faux = {};
    AgentHarness harness = {};
    AgentHarnessConfig config = FauxConfig(&faux, nullptr, 0);
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    AgentHarnessSnapshot snapshot = {};
    AgentHarnessWatch watch = {};
    CHECK(SessionResult_IsOk(
        AgentHarness_Watch(&arena, &harness, 4, &snapshot, &watch)));
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
    CHECK(AgentHarness_WatchClose(&watch).Status == SessionStatus::Busy);
    Thread_Join(&thread);
    CHECK(wait.Result.Status == SessionStatus::InvalidState);
    CHECK(SessionResult_IsOk(AgentHarness_WatchClose(&watch)));
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Model changes are durable settings and release their admission reservation.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    Faux first = {};
    Faux second = {};
    AgentHarness harness = {};
    AgentHarnessConfig config = FauxConfig(&first, nullptr, 0);
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    Model model = {};
    model.Provider = SVL("faux-next");
    model.Id = SVL("model-next");
    Provider provider = Faux_Provider(&second);
    CHECK(SessionResult_IsOk(
        AgentHarness_SetModel(&harness, model, provider)));
    SessionReduction state = {};
    CHECK(SessionResult_IsOk(
        AgentHarness_Snapshot(&arena, &harness, &state)));
    CHECK_ALL(!harness.Running,
              StrView_Eq(state.Provider, model.Provider),
              StrView_Eq(state.Model, model.Id),
              StrView_Eq(harness.Config.ModelInfo.Id, model.Id));
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Awaited hooks may alter accepted input, request context, provider settings,
  // and tool arguments; all effect-facing values are applied before intents.
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
    echo.ReplayPolicy = ToolReplayPolicy::ReplaySafe;
    echo.Execute = EchoExec;
    FauxStep steps[2] = {};
    steps[0].ToolName = SVL("echo");
    steps[0].ToolId = SVL("hook-call");
    steps[0].ToolArgsJson = SVL("{}");
    steps[1].Text = SVL("hooked run complete");
    steps[1].Stop = StopReason::Stop;
    Faux faux = {};
    faux.Steps = steps;
    faux.Count = 2;
    HarnessHookState hookState = {};
    AgentHarness harness = {};
    AgentHarnessConfig config = FauxConfig(&faux, &echo, 1);
    config.Hooks.User = &hookState;
    config.Hooks.BeforeRun = HookBeforeRun;
    config.Hooks.BeforeContext = HookBeforeContext;
    config.Hooks.BeforeProvider = HookBeforeProvider;
    config.Hooks.AfterProvider = HookAfterProvider;
    config.Hooks.BeforeTool = HookBeforeTool;
    config.Hooks.BeforeOperationComplete = HookBeforeComplete;
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    CHECK(SessionResult_IsOk(
        AgentHarness_Run(&harness, SVL("original prompt"))));

    SessionReduction snapshot = {};
    CHECK(SessionResult_IsOk(
        AgentHarness_Snapshot(&arena, &harness, &snapshot)));
    Array<Message> context = {};
    CHECK(SessionResult_IsOk(SessionReducer_Context(
        &arena, &snapshot, snapshot.HeadId, &context)));
    CHECK_ALL(
        context.Count == 4,
        ContextTextEq(&context, 0, Role::User, "hooked durable prompt"),
        ContextTextEq(&context, 2, Role::ToolResult,
                      "echo:{\"hooked\":true}"),
        ContextTextEq(&context, 3, Role::Assistant,
                      "hooked run complete"),
        hookState.BeforeRunCount == 1,
        hookState.BeforeContextCount == 2,
        hookState.BeforeProviderCount == 2,
        hookState.AfterProviderCount == 2,
        hookState.BeforeToolCount == 1,
        hookState.CompletionCount == 1,
        hookState.FirstContextCount == 2,
        hookState.LastContextCount == 4);

    Array<SessionRecord> records = {};
    CHECK(SessionResult_IsOk(
        SessionLog_ReadAll(&harness.Log, &arena, &records)));
    u32 generationCount = 0;
    bool effectiveToolArguments = false;
    for (u32 i = 0; i < records.Count; i++) {
      if (records.Data[i].Kind == SessionRecordKind::GenerationStarted) {
        generationCount++;
        CHECK_ALL(
            StrView_EqCStr(records.Data[i].GenerationStarted.Provider,
                           "hook-provider"),
            StrView_EqCStr(records.Data[i].GenerationStarted.Model,
                           "hook-model"),
            records.Data[i].GenerationStarted.MaxTokens == 321,
            StrView_Contains(
                records.Data[i].GenerationStarted.EffectiveRequestJson,
                "\"model\":\"hook-model\""));
      }
      if (records.Data[i].Kind == SessionRecordKind::ToolStarted &&
          StrView_EqCStr(
              records.Data[i].ToolStarted.EffectiveArgumentsJson,
              "{\"hooked\":true}"))
        effectiveToolArguments = true;
    }
    CHECK_ALL(generationCount == 2, effectiveToolArguments);
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Hook failure becomes a typed failed operation while still completing the
  // assistant/tool-result group without executing the blocked tool effect.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    u32 executions = 0;
    Tool tool = {};
    tool.Name = SVL("recover");
    tool.Description = SVL("must not execute");
    tool.ParamsJsonSchema = SVL("{\"type\":\"object\"}");
    tool.ReplayPolicy = ToolReplayPolicy::ManualReconcile;
    tool.Ctx = &executions;
    tool.Execute = CountReplayExec;
    FauxStep step = {};
    step.ToolName = SVL("recover");
    step.ToolId = SVL("blocked-call");
    step.ToolArgsJson = SVL("{}");
    Faux faux = {};
    faux.Steps = &step;
    faux.Count = 1;
    u32 hookCalls = 0;
    AgentHarness harness = {};
    AgentHarnessConfig config = FauxConfig(&faux, &tool, 1);
    config.Hooks.User = &hookCalls;
    config.Hooks.BeforeTool = FailBeforeTool;
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    CHECK(AgentHarness_Run(&harness, SVL("block tool")).Status ==
          SessionStatus::HookError);
    SessionReduction snapshot = {};
    CHECK(SessionResult_IsOk(
        AgentHarness_Snapshot(&arena, &harness, &snapshot)));
    Array<Message> context = {};
    CHECK(SessionResult_IsOk(SessionReducer_Context(
        &arena, &snapshot, snapshot.HeadId, &context)));
    Array<SessionRecord> records = {};
    CHECK(SessionResult_IsOk(
        SessionLog_ReadAll(&harness.Log, &arena, &records)));
    CHECK_ALL(
        hookCalls == 1, executions == 0, context.Count == 3,
        context.Data[2].Kind == Role::ToolResult, context.Data[2].IsError,
        ContextTextEq(&context, 2, Role::ToolResult, "tool hook failed"),
        CountKind(&records, SessionRecordKind::ToolStarted) == 1,
        CountKind(&records, SessionRecordKind::ToolFinished) == 1,
        CountKind(&records, SessionRecordKind::OperationFinished) == 1,
        !snapshot.HasActiveOperation);
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Steering accepted while a tool effect is active is persisted first, then
  // committed after the complete assistant/tool-result group.
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
    steer.ReplayPolicy = ToolReplayPolicy::ManualReconcile;
    steer.Execute = SteerExec;
    FauxStep steps[2] = {};
    steps[0].ToolName = SVL("steer");
    steps[0].ToolId = SVL("call-steer");
    steps[0].ToolArgsJson = SVL("{}");
    steps[1].Text = SVL("after steering");
    steps[1].Stop = StopReason::Stop;
    Faux faux = {};
    faux.Steps = steps;
    faux.Count = 2;

    AgentHarness harness = {};
    steer.Ctx = &harness;
    AgentHarnessConfig config = FauxConfig(&faux, &steer, 1);
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("start"))));

    SessionReduction snapshot = {};
    CHECK(SessionResult_IsOk(
        AgentHarness_Snapshot(&arena, &harness, &snapshot)));
    Array<Message> context = {};
    CHECK(SessionResult_IsOk(SessionReducer_Context(
        &arena, &snapshot, snapshot.HeadId, &context)));
    CHECK_ALL(context.Count == 5,
              context.Data[1].Kind == Role::Assistant,
              ContextTextEq(&context, 2, Role::ToolResult, "queued"),
              ContextTextEq(&context, 3, Role::User, "steered durably"),
              ContextTextEq(&context, 4, Role::Assistant, "after steering"),
              snapshot.PendingQueueItems.Count == 0,
              snapshot.PendingDeferredWrites.Count == 0);

    Array<SessionRecord> records = {};
    CHECK(SessionResult_IsOk(
        SessionLog_ReadAll(&harness.Log, &arena, &records)));
    CHECK_ALL(CountKind(&records, SessionRecordKind::QueueItemAccepted) == 1,
              CountKind(&records, SessionRecordKind::DeferredWrite) == 1,
              CountKind(&records, SessionRecordKind::DeferredWriteCommitted) == 1,
              CountKind(&records, SessionRecordKind::QueueCheckpoint) == 1);

    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Append failure faults the harness before the provider effect starts.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    FauxStep step = {};
    step.Text = SVL("must not run");
    Faux faux = {};
    faux.Steps = &step;
    faux.Count = 1;
    AgentHarness harness = {};
    AgentHarnessConfig config = FauxConfig(&faux, nullptr, 0);
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    MemoryRepository_FailAfterAppends(&memory, 0);
    CHECK(AgentHarness_Run(&harness, SVL("fail before effect")).Status ==
          SessionStatus::IoError);
    CHECK_ALL(faux.Index == 0, harness.Faulted);
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // A deferred message and commit that reached storage without their queue
  // checkpoint are checkpointed on resume without duplicating the message.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    CHECK(SessionResult_IsOk(AppendTornDeferredCheckpoint(&log, &arena)));
    FauxStep step = {};
    step.Text = SVL("after torn checkpoint");
    step.Stop = StopReason::Stop;
    Faux faux = {};
    faux.Steps = &step;
    faux.Count = 1;
    AgentHarness harness = {};
    AgentHarnessConfig config = FauxConfig(&faux, nullptr, 0);
    CHECK(AgentHarness_Init(&harness, log, &config).Status ==
          SessionStatus::Suspended);
    CHECK(SessionResult_IsOk(AgentHarness_ResumeOperation(&harness)));
    SessionReduction snapshot = {};
    CHECK(SessionResult_IsOk(
        AgentHarness_Snapshot(&arena, &harness, &snapshot)));
    Array<Message> context = {};
    CHECK(SessionResult_IsOk(SessionReducer_Context(
        &arena, &snapshot, snapshot.HeadId, &context)));
    Array<SessionRecord> records = {};
    CHECK(SessionResult_IsOk(
        SessionLog_ReadAll(&harness.Log, &arena, &records)));
    CHECK_ALL(context.Count == 3,
              ContextTextEq(&context, 0, Role::User,
                            "prompt before queued steering"),
              ContextTextEq(&context, 1, Role::User, "queued once"),
              ContextTextEq(&context, 2, Role::Assistant,
                            "after torn checkpoint"),
              CountKind(&records, SessionRecordKind::QueueCheckpoint) == 1,
              CountKind(&records, SessionRecordKind::Message) == 3);
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Cancellation is accepted durably before the provider sees the abort flag.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    CancelProviderState providerState = {};
    AgentHarnessConfig config = {};
    config.SystemPrompt = SVL("cancel test");
    config.ModelInfo.Id = SVL("cancel-model");
    config.ModelInfo.Provider = SVL("test");
    config.ProviderImpl.Ctx = &providerState;
    config.ProviderImpl.Stream = CancelProviderStream;
    config.ToolExec = ToolExecution::Parallel;
    AgentHarness harness = {};
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    HarnessRunThread run = {};
    run.Harness = &harness;
    Thread thread = {};
    CHECK(Thread_Start(&thread, RunHarnessThread, &run));
    u32 waited = 0;
    while (!providerState.Started && waited < 2000) {
      Sleep_Ms(1);
      waited++;
    }
    CHECK(providerState.Started != 0);
    CHECK(SessionResult_IsOk(AgentHarness_RequestCancel(&harness)));
    Thread_Join(&thread);
    CHECK(run.Result.Status == SessionStatus::Cancelled);
    Array<SessionRecord> records = {};
    CHECK(SessionResult_IsOk(
        SessionLog_ReadAll(&harness.Log, &arena, &records)));
    CHECK_ALL(
        CountKind(&records, SessionRecordKind::OperationCancelRequested) == 1,
        CountKind(&records, SessionRecordKind::OperationFinished) == 1);
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Restore is effect-free and exposes a suspended operation that can be
  // explicitly aborted.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    Array<SessionRecord> initial = {};
    CHECK(SessionResult_IsOk(SessionLog_ReadAll(&log, &arena, &initial)));
    SessionRecord started = {};
    started.EntryId = DurableId_Generate();
    started.Kind = SessionRecordKind::OperationStarted;
    started.OperationStarted.OperationId = DurableId_Generate();
    started.OperationStarted.Operation = OperationKind::Run;
    started.OperationStarted.StartingHeadId = initial.Data[0].EntryId;
    started.OperationStarted.HasAcceptedMessage = true;
    started.OperationStarted.AcceptedMessage =
        Message_User(&arena, SVL("accepted"));
    started.OperationStarted.ProvisionedEntryId = DurableId_Generate();
    CHECK(SessionResult_IsOk(SessionLog_Append(&log, &started)));

    Faux faux = {};
    AgentHarness harness = {};
    AgentHarnessConfig config = FauxConfig(&faux, nullptr, 0);
    CHECK(AgentHarness_Init(&harness, log, &config).Status ==
          SessionStatus::Suspended);
    CHECK_ALL(faux.Index == 0, harness.Suspended);
    CHECK(SessionResult_IsOk(
        AgentHarness_AbortOperation(&harness, SVL("user chose abort"))));
    SessionReduction snapshot = {};
    CHECK(SessionResult_IsOk(
        AgentHarness_Snapshot(&arena, &harness, &snapshot)));
    CHECK(!snapshot.HasActiveOperation);
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Resuming directly after prompt acceptance commits the provisioned prompt
  // before starting a new provider effect.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    Array<SessionRecord> initial = {};
    CHECK(SessionResult_IsOk(SessionLog_ReadAll(&log, &arena, &initial)));
    SessionRecord started = {};
    started.EntryId = DurableId_Generate();
    started.Kind = SessionRecordKind::OperationStarted;
    started.OperationStarted.OperationId = DurableId_Generate();
    started.OperationStarted.Operation = OperationKind::Run;
    started.OperationStarted.StartingHeadId = initial.Data[0].EntryId;
    started.OperationStarted.HasAcceptedMessage = true;
    started.OperationStarted.AcceptedMessage =
        Message_User(&arena, SVL("accepted prompt"));
    started.OperationStarted.ProvisionedEntryId = DurableId_Generate();
    CHECK(SessionResult_IsOk(SessionLog_Append(&log, &started)));

    FauxStep step = {};
    step.Text = SVL("resumed");
    step.Stop = StopReason::Stop;
    Faux faux = {};
    faux.Steps = &step;
    faux.Count = 1;
    AgentHarness harness = {};
    AgentHarnessConfig config = FauxConfig(&faux, nullptr, 0);
    CHECK(AgentHarness_Init(&harness, log, &config).Status ==
          SessionStatus::Suspended);
    CHECK(SessionResult_IsOk(AgentHarness_ResumeOperation(&harness)));
    SessionReduction snapshot = {};
    CHECK(SessionResult_IsOk(
        AgentHarness_Snapshot(&arena, &harness, &snapshot)));
    Array<Message> context = {};
    CHECK(SessionResult_IsOk(SessionReducer_Context(
        &arena, &snapshot, snapshot.HeadId, &context)));
    CHECK_ALL(faux.Index == 1, context.Count == 2,
              ContextTextEq(&context, 0, Role::User, "accepted prompt"),
              ContextTextEq(&context, 1, Role::Assistant, "resumed"));
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // An interrupted provider attempt is durably closed before a fresh attempt.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    DurableId operationId = {};
    CHECK(SessionResult_IsOk(
        AppendSuspendedProvider(&log, &arena, &operationId)));

    FauxStep step = {};
    step.Text = SVL("retried");
    step.Stop = StopReason::Stop;
    Faux faux = {};
    faux.Steps = &step;
    faux.Count = 1;
    AgentHarness harness = {};
    AgentHarnessConfig config = FauxConfig(&faux, nullptr, 0);
    CHECK(AgentHarness_Init(&harness, log, &config).Status ==
          SessionStatus::Suspended);
    CHECK(SessionResult_IsOk(AgentHarness_ResumeOperation(&harness)));
    Array<SessionRecord> records = {};
    CHECK(SessionResult_IsOk(
        SessionLog_ReadAll(&harness.Log, &arena, &records)));
    CHECK_ALL(faux.Index == 1,
              CountKind(&records, SessionRecordKind::GenerationStarted) == 2,
              CountKind(&records, SessionRecordKind::GenerationFinished) == 2);
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // If the assistant message reached durable storage but its generation result
  // did not, resume closes the existing attempt without calling the provider.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    DurableId operationId = {};
    CHECK(SessionResult_IsOk(
        AppendSuspendedProvider(&log, &arena, &operationId)));
    Array<SessionRecord> prefix = {};
    CHECK(SessionResult_IsOk(SessionLog_ReadAll(&log, &arena, &prefix)));
    const SessionRecord* generation = &prefix.Data[prefix.Count - 1];
    SessionRecord assistant = {};
    assistant.EntryId =
        generation->GenerationStarted.ProvisionedAssistantEntryId;
    assistant.Kind = SessionRecordKind::Message;
    assistant.MessageEntry.ParentId = prefix.Data[prefix.Count - 2].EntryId;
    assistant.MessageEntry.Value = Message_AssistantEmpty(&arena);
    assistant.MessageEntry.Value.Stop = StopReason::Stop;
    Array_Push(&assistant.MessageEntry.Value.Content,
               Content_Text(SVL("already durable")));
    CHECK(SessionResult_IsOk(SessionLog_Append(&log, &assistant)));

    FauxStep unused = {};
    unused.Text = SVL("must not run");
    Faux faux = {};
    faux.Steps = &unused;
    faux.Count = 1;
    AgentHarness harness = {};
    AgentHarnessConfig config = FauxConfig(&faux, nullptr, 0);
    CHECK(AgentHarness_Init(&harness, log, &config).Status ==
          SessionStatus::Suspended);
    CHECK(SessionResult_IsOk(AgentHarness_ResumeOperation(&harness)));
    SessionReduction snapshot = {};
    CHECK(SessionResult_IsOk(
        AgentHarness_Snapshot(&arena, &harness, &snapshot)));
    Array<Message> context = {};
    CHECK(SessionResult_IsOk(SessionReducer_Context(
        &arena, &snapshot, snapshot.HeadId, &context)));
    Array<SessionRecord> records = {};
    CHECK(SessionResult_IsOk(
        SessionLog_ReadAll(&harness.Log, &arena, &records)));
    CHECK_ALL(faux.Index == 0, context.Count == 2,
              ContextTextEq(&context, 1, Role::Assistant,
                            "already durable"),
              CountKind(&records, SessionRecordKind::GenerationStarted) == 1,
              CountKind(&records, SessionRecordKind::GenerationFinished) == 1,
              CountKind(&records, SessionRecordKind::OperationFinished) == 1);
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Replay-safe interrupted tools rerun only after explicit resume.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    SuspendedToolTrace trace = {};
    CHECK(SessionResult_IsOk(AppendSuspendedTool(
        &log, &arena, ToolReplayPolicy::ReplaySafe, &trace)));
    u32 executions = 0;
    Tool tool = {};
    tool.Name = SVL("recover");
    tool.Description = SVL("recovery probe");
    tool.ParamsJsonSchema = SVL("{\"type\":\"object\"}");
    tool.ReplayPolicy = ToolReplayPolicy::ReplaySafe;
    tool.Ctx = &executions;
    tool.Execute = CountReplayExec;
    FauxStep step = {};
    step.Text = SVL("after replay");
    step.Stop = StopReason::Stop;
    Faux faux = {};
    faux.Steps = &step;
    faux.Count = 1;
    AgentHarness harness = {};
    AgentHarnessConfig config = FauxConfig(&faux, &tool, 1);
    CHECK(AgentHarness_Init(&harness, log, &config).Status ==
          SessionStatus::Suspended);
    CHECK(executions == 0);
    SessionResult resumed = AgentHarness_ResumeOperation(&harness);
    CHECK(SessionResult_IsOk(resumed));
    CHECK(executions == 1);
    Array<SessionRecord> records = {};
    CHECK(SessionResult_IsOk(
        SessionLog_ReadAll(&harness.Log, &arena, &records)));
    CHECK(CountKind(&records, SessionRecordKind::RecoveryResolved) == 1);
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // A durable tool-result message without its ToolFinished record is completed
  // without rerunning even a manual-reconciliation tool.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    SuspendedToolTrace trace = {};
    CHECK(SessionResult_IsOk(AppendSuspendedTool(
        &log, &arena, ToolReplayPolicy::ManualReconcile, &trace)));
    Array<SessionRecord> prefix = {};
    CHECK(SessionResult_IsOk(SessionLog_ReadAll(&log, &arena, &prefix)));
    const SessionRecord* toolStarted = &prefix.Data[prefix.Count - 1];
    SessionRecord result = {};
    result.EntryId = toolStarted->ToolStarted.ProvisionedResultEntryId;
    result.Kind = SessionRecordKind::Message;
    result.MessageEntry.ParentId = prefix.Data[prefix.Count - 3].EntryId;
    result.MessageEntry.Value = Message_ToolResult(
        &arena, SVL("call-recover"), SVL("recover"),
        SVL("already durable tool result"), false);
    CHECK(SessionResult_IsOk(SessionLog_Append(&log, &result)));

    u32 executions = 0;
    Tool tool = {};
    tool.Name = SVL("recover");
    tool.Description = SVL("manual recovery probe");
    tool.ParamsJsonSchema = SVL("{\"type\":\"object\"}");
    tool.ReplayPolicy = ToolReplayPolicy::ManualReconcile;
    tool.Ctx = &executions;
    tool.Execute = CountReplayExec;
    FauxStep step = {};
    step.Text = SVL("continued after durable tool result");
    step.Stop = StopReason::Stop;
    Faux faux = {};
    faux.Steps = &step;
    faux.Count = 1;
    AgentHarness harness = {};
    AgentHarnessConfig config = FauxConfig(&faux, &tool, 1);
    CHECK(AgentHarness_Init(&harness, log, &config).Status ==
          SessionStatus::Suspended);
    CHECK(SessionResult_IsOk(AgentHarness_ResumeOperation(&harness)));
    SessionReduction snapshot = {};
    CHECK(SessionResult_IsOk(
        AgentHarness_Snapshot(&arena, &harness, &snapshot)));
    Array<Message> context = {};
    CHECK(SessionResult_IsOk(SessionReducer_Context(
        &arena, &snapshot, snapshot.HeadId, &context)));
    Array<SessionRecord> records = {};
    CHECK(SessionResult_IsOk(
        SessionLog_ReadAll(&harness.Log, &arena, &records)));
    CHECK_ALL(
        executions == 0, faux.Index == 1, context.Count == 4,
        ContextTextEq(&context, 2, Role::ToolResult,
                      "already durable tool result"),
        ContextTextEq(&context, 3, Role::Assistant,
                      "continued after durable tool result"),
        CountKind(&records, SessionRecordKind::ToolFinished) == 1,
        CountKind(&records, SessionRecordKind::RecoveryResolved) == 0);
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Non-replayable tools require an explicit reconciliation decision. Recording
  // failure resumes without executing the uncertain effect.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    SuspendedToolTrace trace = {};
    CHECK(SessionResult_IsOk(AppendSuspendedTool(
        &log, &arena, ToolReplayPolicy::ManualReconcile, &trace)));
    u32 executions = 0;
    Tool tool = {};
    tool.Name = SVL("recover");
    tool.Description = SVL("manual recovery probe");
    tool.ParamsJsonSchema = SVL("{\"type\":\"object\"}");
    tool.ReplayPolicy = ToolReplayPolicy::ManualReconcile;
    tool.Ctx = &executions;
    tool.Execute = CountReplayExec;
    FauxStep step = {};
    step.Text = SVL("after reconciliation");
    step.Stop = StopReason::Stop;
    Faux faux = {};
    faux.Steps = &step;
    faux.Count = 1;
    AgentHarness harness = {};
    AgentHarnessConfig config = FauxConfig(&faux, &tool, 1);
    CHECK(AgentHarness_Init(&harness, log, &config).Status ==
          SessionStatus::Suspended);
    CHECK(AgentHarness_ResumeOperation(&harness).Status ==
          SessionStatus::RecoveryRequired);
    CHECK(executions == 0);
    CHECK(SessionResult_IsOk(AgentHarness_ReconcileTool(
        &harness, trace.ToolExecutionId, RecoveryResolution::RecordFailure,
        nullptr)));
    CHECK(SessionResult_IsOk(AgentHarness_ResumeOperation(&harness)));
    CHECK(executions == 0);
    SessionReduction snapshot = {};
    CHECK(SessionResult_IsOk(
        AgentHarness_Snapshot(&arena, &harness, &snapshot)));
    Array<Message> context = {};
    CHECK(SessionResult_IsOk(SessionReducer_Context(
        &arena, &snapshot, snapshot.HeadId, &context)));
    CHECK_ALL(context.Count == 4, context.Data[2].Kind == Role::ToolResult,
              context.Data[2].IsError,
              ContextTextEq(&context, 3, Role::Assistant,
                            "after reconciliation"));
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Compaction persists intent before generation, commits the summary before
  // the generation result, and replaces prior model context.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    FauxStep steps[2] = {};
    steps[0].Text = SVL("answer before compaction");
    steps[0].Stop = StopReason::Stop;
    steps[1].Text = SVL("compact summary");
    steps[1].Stop = StopReason::Stop;
    Faux faux = {};
    faux.Steps = steps;
    faux.Count = 2;
    AgentHarness harness = {};
    AgentHarnessConfig config = FauxConfig(&faux, nullptr, 0);
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
    CHECK(SessionResult_IsOk(AgentHarness_Run(&harness, SVL("old prompt"))));
    CHECK(SessionResult_IsOk(AgentHarness_Compact(&harness)));

    SessionReduction snapshot = {};
    CHECK(SessionResult_IsOk(
        AgentHarness_Snapshot(&arena, &harness, &snapshot)));
    Array<Message> context = {};
    CHECK(SessionResult_IsOk(SessionReducer_Context(
        &arena, &snapshot, snapshot.HeadId, &context)));
    CHECK_ALL(
        context.Count == 1,
        ContextTextEq(&context, 0, Role::User,
                      "Summary of previous conversation:\ncompact summary"),
        !snapshot.HasActiveOperation);

    Array<SessionRecord> records = {};
    CHECK(SessionResult_IsOk(
        SessionLog_ReadAll(&harness.Log, &arena, &records)));
    u32 compactionIndex = records.Count;
    for (u32 i = 0; i < records.Count; i++)
      if (records.Data[i].Kind == SessionRecordKind::Compaction)
        compactionIndex = i;
    CHECK_ALL(
        CountKind(&records, SessionRecordKind::Compaction) == 1,
        CountKind(&records, SessionRecordKind::OperationStarted) == 2,
        CountKind(&records, SessionRecordKind::GenerationStarted) == 2,
        CountKind(&records, SessionRecordKind::GenerationFinished) == 2,
        CountKind(&records, SessionRecordKind::OperationFinished) == 2,
        compactionIndex >= 2 && compactionIndex + 2 < records.Count,
        records.Data[compactionIndex - 2].Kind ==
            SessionRecordKind::OperationStarted,
        records.Data[compactionIndex - 1].Kind ==
            SessionRecordKind::GenerationStarted,
        records.Data[compactionIndex + 1].Kind ==
            SessionRecordKind::GenerationFinished,
        records.Data[compactionIndex + 2].Kind ==
            SessionRecordKind::OperationFinished);

    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // An interrupted compaction attempt is closed durably before an explicit
  // resume starts a fresh provider attempt with the provisioned summary ID.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    CHECK(SessionResult_IsOk(AppendSuspendedCompaction(&log, &arena)));
    FauxStep step = {};
    step.Text = SVL("resumed compact summary");
    step.Stop = StopReason::Stop;
    Faux faux = {};
    faux.Steps = &step;
    faux.Count = 1;
    AgentHarness harness = {};
    AgentHarnessConfig config = FauxConfig(&faux, nullptr, 0);
    CHECK(AgentHarness_Init(&harness, log, &config).Status ==
          SessionStatus::Suspended);
    CHECK(faux.Index == 0);
    CHECK(SessionResult_IsOk(AgentHarness_ResumeOperation(&harness)));
    Array<SessionRecord> records = {};
    CHECK(SessionResult_IsOk(
        SessionLog_ReadAll(&harness.Log, &arena, &records)));
    CHECK_ALL(faux.Index == 1,
              CountKind(&records, SessionRecordKind::Compaction) == 1,
              CountKind(&records, SessionRecordKind::GenerationStarted) == 2,
              CountKind(&records, SessionRecordKind::GenerationFinished) == 2,
              CountKind(&records, SessionRecordKind::OperationFinished) == 1);
    SessionReduction snapshot = {};
    CHECK(SessionResult_IsOk(
        AgentHarness_Snapshot(&arena, &harness, &snapshot)));
    Array<Message> context = {};
    CHECK(SessionResult_IsOk(SessionReducer_Context(
        &arena, &snapshot, snapshot.HeadId, &context)));
    CHECK_ALL(
        context.Count == 1,
        ContextTextEq(
            &context, 0, Role::User,
            "Summary of previous conversation:\nresumed compact summary"));
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // A compaction entry that reached storage without its generation result is
  // finalized in place without issuing another provider request.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    CHECK(SessionResult_IsOk(AppendSuspendedCompaction(&log, &arena)));
    Array<SessionRecord> prefix = {};
    CHECK(SessionResult_IsOk(SessionLog_ReadAll(&log, &arena, &prefix)));
    const SessionRecord* operation = &prefix.Data[prefix.Count - 2];
    SessionRecord compaction = {};
    compaction.EntryId = operation->OperationStarted.ProvisionedEntryId;
    compaction.Kind = SessionRecordKind::Compaction;
    compaction.CompactionEntry.ParentId =
        operation->OperationStarted.StartingHeadId;
    compaction.CompactionEntry.BaseEntryId = prefix.Data[0].EntryId;
    compaction.CompactionEntry.ThroughEntryId =
        operation->OperationStarted.StartingHeadId;
    compaction.CompactionEntry.SourceHeadId =
        operation->OperationStarted.StartingHeadId;
    compaction.CompactionEntry.Summary =
        Message_User(&arena, SVL("Summary of previous conversation:\ndurable"));
    CHECK(SessionResult_IsOk(SessionLog_Append(&log, &compaction)));

    FauxStep unused = {};
    unused.Text = SVL("must not run");
    Faux faux = {};
    faux.Steps = &unused;
    faux.Count = 1;
    AgentHarness harness = {};
    AgentHarnessConfig config = FauxConfig(&faux, nullptr, 0);
    CHECK(AgentHarness_Init(&harness, log, &config).Status ==
          SessionStatus::Suspended);
    CHECK(SessionResult_IsOk(AgentHarness_ResumeOperation(&harness)));
    SessionReduction snapshot = {};
    CHECK(SessionResult_IsOk(
        AgentHarness_Snapshot(&arena, &harness, &snapshot)));
    Array<Message> context = {};
    CHECK(SessionResult_IsOk(SessionReducer_Context(
        &arena, &snapshot, snapshot.HeadId, &context)));
    Array<SessionRecord> records = {};
    CHECK(SessionResult_IsOk(
        SessionLog_ReadAll(&harness.Log, &arena, &records)));
    CHECK_ALL(
        faux.Index == 0, context.Count == 1,
        ContextTextEq(&context, 0, Role::User,
                      "Summary of previous conversation:\ndurable"),
        CountKind(&records, SessionRecordKind::Compaction) == 1,
        CountKind(&records, SessionRecordKind::GenerationStarted) == 1,
        CountKind(&records, SessionRecordKind::GenerationFinished) == 1,
        CountKind(&records, SessionRecordKind::OperationFinished) == 1);
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }

  // Navigation restores ancestry-derived settings, and the next run branches
  // from the checked-out head. Harness records are excluded from a fork.
  {
    Arena arena = ArenaCreate();
    MemoryRepository memory = {};
    MemoryRepository_Init(&memory);
    SessionRepository repository = MemoryRepository_AsRepository(&memory);
    SessionLog log = {};
    CHECK(SessionResult_IsOk(CreateSession(&repository, &arena, &log)));
    FauxStep step = {};
    step.Text = SVL("branched response");
    step.Stop = StopReason::Stop;
    Faux faux = {};
    faux.Steps = &step;
    faux.Count = 1;
    AgentHarness harness = {};
    AgentHarnessConfig config = FauxConfig(&faux, nullptr, 0);
    CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));

    SessionReduction initial = {};
    CHECK(SessionResult_IsOk(
        AgentHarness_Snapshot(&arena, &harness, &initial)));
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
    entries[2].ToolsEntry.ToolNames.Data = oldToolNames;
    entries[2].ToolsEntry.ToolNames.Count = 1;
    entries[2].ToolsEntry.ToolNames.Capacity = 1;
    entries[3].EntryId = DurableId_Generate();
    entries[3].Kind = SessionRecordKind::Message;
    entries[3].MessageEntry.ParentId = entries[2].EntryId;
    entries[3].MessageEntry.Value =
        Message_User(&arena, SVL("old branch message"));
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
    entries[6].ToolsEntry.ToolNames.Data = newToolNames;
    entries[6].ToolsEntry.ToolNames.Count = 1;
    entries[6].ToolsEntry.ToolNames.Capacity = 1;
    entries[7].EntryId = DurableId_Generate();
    entries[7].Kind = SessionRecordKind::Message;
    entries[7].MessageEntry.ParentId = entries[6].EntryId;
    entries[7].MessageEntry.Value =
        Message_User(&arena, SVL("new branch message"));
    CHECK(SessionResult_IsOk(
        SessionLog_AppendBatch(&harness.Log, entries, 8)));
    CHECK(SessionResult_IsOk(
        AgentHarness_Navigate(&harness, entries[7].EntryId)));

    SessionReduction beforeNavigation = {};
    CHECK(SessionResult_IsOk(AgentHarness_Snapshot(
        &arena, &harness, &beforeNavigation)));
    CHECK_ALL(
        StrView_EqCStr(beforeNavigation.Provider, "provider-new"),
        StrView_EqCStr(beforeNavigation.Model, "model-new"),
        StrView_EqCStr(beforeNavigation.ThinkingLevel, "low"),
        beforeNavigation.ToolNames.Count == 1,
        StrView_EqCStr(beforeNavigation.ToolNames.Data[0], "write"));

    CHECK(SessionResult_IsOk(
        AgentHarness_Navigate(&harness, entries[3].EntryId)));
    SessionReduction afterNavigation = {};
    CHECK(SessionResult_IsOk(AgentHarness_Snapshot(
        &arena, &harness, &afterNavigation)));
    CHECK_ALL(
        DurableId_Eq(afterNavigation.HeadId, entries[3].EntryId),
        StrView_EqCStr(afterNavigation.Provider, "provider-old"),
        StrView_EqCStr(afterNavigation.Model, "model-old"),
        StrView_EqCStr(afterNavigation.ThinkingLevel, "high"),
        afterNavigation.ToolNames.Count == 1,
        StrView_EqCStr(afterNavigation.ToolNames.Data[0], "read"));

    CHECK(SessionResult_IsOk(
        AgentHarness_Run(&harness, SVL("continue old branch"))));
    SessionReduction branched = {};
    CHECK(SessionResult_IsOk(
        AgentHarness_Snapshot(&arena, &harness, &branched)));
    Array<Message> context = {};
    CHECK(SessionResult_IsOk(SessionReducer_Context(
        &arena, &branched, branched.HeadId, &context)));
    CHECK_ALL(context.Count == 3,
              ContextTextEq(&context, 0, Role::User, "old branch message"),
              ContextTextEq(&context, 1, Role::User, "continue old branch"),
              ContextTextEq(&context, 2, Role::Assistant,
                            "branched response"),
              StrView_EqCStr(branched.Provider, "provider-old"),
              StrView_EqCStr(branched.Model, "model-old"));
    CHECK(SessionResult_IsOk(
        AgentHarness_LabelBranch(&harness, SVL("old-branch"))));
    SessionReduction labeled = {};
    CHECK(SessionResult_IsOk(
        AgentHarness_Snapshot(&arena, &harness, &labeled)));
    CHECK_ALL(labeled.Branch.Data[labeled.Branch.Count - 1]->Kind ==
                  SessionRecordKind::BranchLabel,
              StrView_EqCStr(
                  labeled.Branch.Data[labeled.Branch.Count - 1]
                      ->BranchLabelEntry.Label,
                  "old-branch"));

    SessionLog forkLog = {};
    SessionInfo forkInfo = {};
    CHECK(SessionResult_IsOk(AgentHarness_Fork(
        &arena, &harness, &repository, labeled.HeadId, DurableId{},
        SVL("branched fork"), &forkLog, &forkInfo)));
    Array<SessionRecord> forkRecords = {};
    CHECK(SessionResult_IsOk(
        SessionLog_ReadAll(&forkLog, &arena, &forkRecords)));
    bool onlySessionEntries = true;
    for (u32 i = 0; i < forkRecords.Count; i++)
      if (forkRecords.Data[i].EntryClass != SessionEntryClass::Session)
        onlySessionEntries = false;
    CHECK_ALL(onlySessionEntries, forkRecords.Count == 8,
              DurableId_Eq(
                  forkRecords.Data[0].SessionStarted.ForkedFromSessionId,
                  sourceSessionId),
              DurableId_Eq(
                  forkRecords.Data[0].SessionStarted.ForkedFromEntryId,
                  labeled.HeadId));
    SessionLog_Close(&forkLog);

    Array<SessionRecord> sourceRecords = {};
    CHECK(SessionResult_IsOk(
        SessionLog_ReadAll(&harness.Log, &arena, &sourceRecords)));
    CHECK_ALL(CountKind(&sourceRecords, SessionRecordKind::OperationStarted) == 3,
              CountKind(&sourceRecords, SessionRecordKind::OperationFinished) ==
                  3);
    CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
    repository.Close(repository.Ctx);
    ArenaRelease(&arena);
  }
}
