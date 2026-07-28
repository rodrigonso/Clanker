#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent/harness.h"
#include "base/arena.h"
#include "base/config_file.h"
#include "check.h"
#include "session/entry.h"
#include "session/jsonl_repository.h"
#include "session/metadata.h"
#include "session/memory_repository.h"
#include "session/reducer.h"
#include "tests.h"
#include "tools/fs.h"

#if defined(_WIN32)
#include <direct.h>
#define SESSION_RMDIR _rmdir
#else
#include <unistd.h>
#define SESSION_RMDIR rmdir
#endif

static DurableId TestId(u8 seed) {
  byte bytes[DURABLE_ID_BYTE_COUNT];
  for (u32 i = 0; i < DURABLE_ID_BYTE_COUNT; i++) bytes[i] = (byte)(seed + i);
  return DurableId_FromBytes(bytes);
}

static SessionRecord BaseRecord(DurableId sessionId, u64 seq, u8 idSeed,
                                SessionRecordKind kind) {
  SessionRecord record = {};
  record.SchemaVersion = SESSION_SCHEMA_VERSION;
  record.SessionId = sessionId;
  record.Seq = seq;
  record.EntryId = TestId(idSeed);
  record.EntryClass = SessionRecord_ClassForKind(kind);
  record.Kind = kind;
  record.CreatedAt = 1700000000000ull + seq;
  return record;
}

static Message UserMessage(Arena* arena, const char* text) {
  Message message = Message_User(arena, SV(text));
  message.Timestamp = 1700000000000ull;
  return message;
}

static Message AssistantToolMessage(Arena* arena) {
  Message message = Message_AssistantEmpty(arena);
  message.Timestamp = 1700000000001ull;
  message.Stop = StopReason::ToolUse;
  message.Use.Input = 10;
  message.Use.Output = 2;
  message.Use.CacheRead = 3;
  message.Use.CacheWrite = 4;
  message.Use.Reasoning = 1;
  message.Model = SVL("claude-test");
  message.Provider = SVL("anthropic");
  Array_Push(&message.Content, Content_Text(SVL("checking")));
  Array_Push(&message.Content,
             Content_ToolCall(SVL("call-1"), SVL("read"), SVL("{\"path\":\"a.txt\"}")));
  return message;
}

static void TestDurableIds() {
  DurableId id = TestId(3);
  CHECK_ALL(DurableId_IsSet(id), DurableId_View(&id).Length == DURABLE_ID_CHAR_COUNT);

  DurableId parsed = {};
  CHECK(DurableId_Parse(DurableId_View(&id), &parsed));
  CHECK(DurableId_Eq(id, parsed));
  CHECK(!DurableId_Parse(SVL("short"), &parsed));
  CHECK(!DurableId_Parse(SVL("!!!!!!!!!!!!!!!!!!!!!!"), &parsed));

  DurableId random = DurableId_Generate();
  CHECK(DurableId_IsSet(random));
}

static void TestRecordCodec(Arena* arena) {
  DurableId sessionId = TestId(1);
  SessionRecord source =
      BaseRecord(sessionId, 4, 20, SessionRecordKind::GenerationStarted);
  source.GenerationStarted.OperationId = TestId(9);
  source.GenerationStarted.AttemptId = TestId(10);
  source.GenerationStarted.Provider = SVL("anthropic");
  source.GenerationStarted.Model = SVL("claude-test");
  source.GenerationStarted.EffectiveRequestJson =
      SVL("{\"temperature\":0,\"system\":\"durable\"}");
  source.GenerationStarted.MaxTokens = 4096;
  source.GenerationStarted.ProvisionedAssistantEntryId = TestId(21);

  StrBuf encoded = StrBuf_Make(arena, 512);
  SessionResult write = SessionRecord_Encode(&encoded, &source);
  CHECK(SessionResult_IsOk(write));

  SessionRecord decoded = {};
  SessionResult read = SessionRecord_Decode(arena, StrBuf_View(&encoded), &decoded);
  CHECK(SessionResult_IsOk(read));
  CHECK_ALL(decoded.Kind == SessionRecordKind::GenerationStarted,
            decoded.EntryClass == SessionEntryClass::Harness,
            decoded.Seq == source.Seq,
            DurableId_Eq(decoded.SessionId, source.SessionId),
            DurableId_Eq(decoded.GenerationStarted.AttemptId,
                         source.GenerationStarted.AttemptId),
            decoded.GenerationStarted.MaxTokens == 4096,
            StrView_Eq(decoded.GenerationStarted.EffectiveRequestJson,
                       source.GenerationStarted.EffectiveRequestJson));

  StrBuf bad = StrBuf_Make(arena, encoded.Length + 8);
  StrBuf_Append(&bad, StrBuf_View(&encoded));
  StrBuf_AppendCStr(&bad, " trailing");
  CHECK(SessionRecord_Decode(arena, StrBuf_View(&bad), &decoded).Status ==
        SessionStatus::Corrupt);

  const char* unknown =
      "{\"schemaVersion\":1,\"sessionId\":\"AQIDBAUGBwgJCgsMDQ4PEA\",\"seq\":1,"
      "\"entryId\":\"AgMEBQYHCAkKCwwNDg8QEQ\",\"entryClass\":\"session\","
      "\"type\":\"future_type\",\"createdAt\":1,\"payload\":{}}";
  CHECK(SessionRecord_Decode(arena, SV(unknown), &decoded).Status ==
        SessionStatus::Corrupt);

  const char* future =
      "{\"schemaVersion\":2,\"sessionId\":\"AQIDBAUGBwgJCgsMDQ4PEA\",\"seq\":1,"
      "\"entryId\":\"AgMEBQYHCAkKCwwNDg8QEQ\",\"entryClass\":\"session\","
      "\"type\":\"session_started\",\"createdAt\":1,\"payload\":{}}";
  CHECK(SessionRecord_Decode(arena, SV(future), &decoded).Status ==
        SessionStatus::UnsupportedVersion);

  SessionRecord finished =
      BaseRecord(sessionId, 5, 22, SessionRecordKind::GenerationFinished);
  finished.GenerationFinished.OperationId = TestId(9);
  finished.GenerationFinished.AttemptId = TestId(10);
  finished.GenerationFinished.Outcome = GenerationOutcome::Failed;
  finished.GenerationFinished.Error = SVL("retry");
  finished.GenerationFinished.HasUsage = true;
  finished.GenerationFinished.Use =
      Usage{11, 12, 13, 14, 15, 65};
  StrBuf finishedJson = StrBuf_Make(arena, 512);
  CHECK(SessionResult_IsOk(
      SessionRecord_Encode(&finishedJson, &finished)));
  CHECK(StrView_Contains(StrBuf_View(&finishedJson), "\"usage\""));
  CHECK(SessionResult_IsOk(SessionRecord_Decode(
      arena, StrBuf_View(&finishedJson), &decoded)));
  CHECK_ALL(decoded.GenerationFinished.HasUsage,
            decoded.GenerationFinished.Use.Input == 11,
            decoded.GenerationFinished.Use.TotalTokens == 65);

  const char* oldFinished =
      "{\"schemaVersion\":1,\"sessionId\":\"AQIDBAUGBwgJCgsMDQ4PEA\",\"seq\":5,"
      "\"entryId\":\"FhcYGRobHB0eHyAhIiMkJQ\",\"entryClass\":\"harness\","
      "\"type\":\"generation_finished\",\"createdAt\":5,\"payload\":{"
      "\"operationId\":\"CQoLDA0ODxAREhMUFRYXGA\","
      "\"attemptId\":\"CgsMDQ4PEBESExQVFhcYGQ\","
      "\"outcome\":\"failed\",\"messageEntryId\":null,\"error\":\"old\"}}";
  CHECK(SessionResult_IsOk(
      SessionRecord_Decode(arena, SV(oldFinished), &decoded)));
  CHECK(!decoded.GenerationFinished.HasUsage);

  SessionRecord named =
      BaseRecord(sessionId, 6, 23, SessionRecordKind::SessionName);
  named.SessionNameEntry.Name = {};
  StrBuf namedJson = StrBuf_Make(arena, 256);
  CHECK(SessionResult_IsOk(SessionRecord_Encode(&namedJson, &named)));
  CHECK(SessionResult_IsOk(
      SessionRecord_Decode(arena, StrBuf_View(&namedJson), &decoded)));
  CHECK_ALL(decoded.Kind == SessionRecordKind::SessionName,
            decoded.EntryClass == SessionEntryClass::Session,
            decoded.SessionNameEntry.Name.Length == 0);
}

static Array<SessionRecord> BuildValidTrace(Arena* arena) {
  DurableId sessionId = TestId(1);
  DurableId rootId = TestId(2);
  DurableId operationId = TestId(3);
  DurableId userId = TestId(4);
  DurableId attemptId = TestId(5);
  DurableId assistantId = TestId(6);
  DurableId executionId = TestId(7);
  DurableId toolResultId = TestId(8);

  Array<SessionRecord> records = Array_Make<SessionRecord>(arena, 10);

  SessionRecord started =
      BaseRecord(sessionId, 1, 2, SessionRecordKind::SessionStarted);
  started.EntryId = rootId;
  started.SessionStarted.WorkingDirectory = SVL("C:\\work\\clank");
  started.SessionStarted.ProjectKey = SVL("clank-1234");
  started.SessionStarted.Label = SVL("test");
  Array_Push(&records, started);

  SessionRecord operation =
      BaseRecord(sessionId, 2, 30, SessionRecordKind::OperationStarted);
  operation.OperationStarted.OperationId = operationId;
  operation.OperationStarted.Operation = OperationKind::Run;
  operation.OperationStarted.StartingHeadId = rootId;
  operation.OperationStarted.HasAcceptedMessage = true;
  operation.OperationStarted.AcceptedMessage = UserMessage(arena, "hello");
  operation.OperationStarted.ProvisionedEntryId = userId;
  Array_Push(&records, operation);

  SessionRecord user = BaseRecord(sessionId, 3, 4, SessionRecordKind::Message);
  user.EntryId = userId;
  user.MessageEntry.ParentId = rootId;
  user.MessageEntry.Value = UserMessage(arena, "hello");
  Array_Push(&records, user);

  SessionRecord generation =
      BaseRecord(sessionId, 4, 31, SessionRecordKind::GenerationStarted);
  generation.GenerationStarted.OperationId = operationId;
  generation.GenerationStarted.AttemptId = attemptId;
  generation.GenerationStarted.Provider = SVL("anthropic");
  generation.GenerationStarted.Model = SVL("claude-test");
  generation.GenerationStarted.EffectiveRequestJson = SVL("{\"maxTokens\":1024}");
  generation.GenerationStarted.MaxTokens = 1024;
  generation.GenerationStarted.ProvisionedAssistantEntryId = assistantId;
  Array_Push(&records, generation);

  SessionRecord assistant =
      BaseRecord(sessionId, 5, 6, SessionRecordKind::Message);
  assistant.EntryId = assistantId;
  assistant.MessageEntry.ParentId = userId;
  assistant.MessageEntry.Value = AssistantToolMessage(arena);
  Array_Push(&records, assistant);

  SessionRecord generationDone =
      BaseRecord(sessionId, 6, 32, SessionRecordKind::GenerationFinished);
  generationDone.GenerationFinished.OperationId = operationId;
  generationDone.GenerationFinished.AttemptId = attemptId;
  generationDone.GenerationFinished.Outcome = GenerationOutcome::Succeeded;
  generationDone.GenerationFinished.MessageEntryId = assistantId;
  Array_Push(&records, generationDone);

  SessionRecord tool =
      BaseRecord(sessionId, 7, 33, SessionRecordKind::ToolStarted);
  tool.ToolStarted.OperationId = operationId;
  tool.ToolStarted.ToolExecutionId = executionId;
  tool.ToolStarted.ToolCallId = SVL("call-1");
  tool.ToolStarted.ToolName = SVL("read");
  tool.ToolStarted.EffectiveArgumentsJson = SVL("{\"path\":\"a.txt\"}");
  tool.ToolStarted.ProvisionedResultEntryId = toolResultId;
  tool.ToolStarted.ReplayPolicy = ToolReplayPolicy::ReplaySafe;
  Array_Push(&records, tool);

  SessionRecord result = BaseRecord(sessionId, 8, 8, SessionRecordKind::Message);
  result.EntryId = toolResultId;
  result.MessageEntry.ParentId = assistantId;
  result.MessageEntry.Value =
      Message_ToolResult(arena, SVL("call-1"), SVL("read"), SVL("contents"), false);
  result.MessageEntry.Value.Timestamp = 1700000000002ull;
  Array_Push(&records, result);

  SessionRecord toolDone =
      BaseRecord(sessionId, 9, 34, SessionRecordKind::ToolFinished);
  toolDone.ToolFinished.OperationId = operationId;
  toolDone.ToolFinished.ToolExecutionId = executionId;
  toolDone.ToolFinished.Outcome = ToolOutcomeKind::Succeeded;
  toolDone.ToolFinished.ResultEntryId = toolResultId;
  toolDone.ToolFinished.Terminate = false;
  Array_Push(&records, toolDone);

  SessionRecord operationDone =
      BaseRecord(sessionId, 10, 35, SessionRecordKind::OperationFinished);
  operationDone.OperationFinished.OperationId = operationId;
  operationDone.OperationFinished.Outcome = OperationOutcome::Succeeded;
  operationDone.OperationFinished.ResultHeadId = toolResultId;
  Array_Push(&records, operationDone);

  return records;
}

static void TestRecordValidation(Arena* arena) {
  Array<SessionRecord> records = BuildValidTrace(arena);
  SessionResult valid = SessionRecords_Validate(records.Data, records.Count);
  CHECK_ALL(SessionResult_IsOk(valid), valid.Seq == records.Count);

  records.Data[4].MessageEntry.ParentId = TestId(99);
  CHECK(SessionRecords_Validate(records.Data, records.Count).Status ==
        SessionStatus::Corrupt);
  records.Data[4].MessageEntry.ParentId = records.Data[2].EntryId;

  records.Data[5].GenerationFinished.MessageEntryId = TestId(98);
  CHECK(SessionRecords_Validate(records.Data, records.Count).Status ==
        SessionStatus::Corrupt);
  records.Data[5].GenerationFinished.MessageEntryId = records.Data[4].EntryId;

  records.Data[8].Seq = 12;
  CHECK(SessionRecords_Validate(records.Data, records.Count).Status ==
        SessionStatus::Corrupt);
}

static void TestReducer(Arena* arena) {
  Array<SessionRecord> records = BuildValidTrace(arena);
  SessionReduction reduction = {};
  SessionResult reduced =
      SessionReducer_Reduce(arena, records.Data, records.Count, &reduction);
  CHECK(SessionResult_IsOk(reduced));
  CHECK_ALL(!reduction.HasActiveOperation,
            reduction.Suspension == SessionSuspensionKind::None,
            DurableId_Eq(reduction.HeadId, records.Data[7].EntryId),
            reduction.Branch.Count == 4);

  Array<Message> context = {};
  CHECK(SessionResult_IsOk(
      SessionReducer_Context(arena, &reduction, reduction.HeadId, &context)));
  CHECK_ALL(context.Count == 3, context.Data[0].Kind == Role::User,
            context.Data[1].Kind == Role::Assistant,
            context.Data[2].Kind == Role::ToolResult);

  SessionReduction suspended = {};
  CHECK(SessionResult_IsOk(
      SessionReducer_Reduce(arena, records.Data, 4, &suspended)));
  CHECK_ALL(suspended.HasActiveOperation,
            suspended.Suspension == SessionSuspensionKind::Generation,
            DurableId_Eq(suspended.PendingAttemptId,
                         records.Data[3].GenerationStarted.AttemptId),
            DurableId_Eq(suspended.HeadId, records.Data[2].EntryId));
}

static void TestMetadata(Arena* arena) {
  Array<SessionRecord> records = BuildValidTrace(arena);
  SessionMetadataBuildOptions legacyOptions = {};
  SessionInfo legacy = {};
  CHECK(SessionResult_IsOk(SessionMetadata_Build(
      arena, records.Data, records.Count, &legacyOptions, &legacy)));
  CHECK_ALL(legacy.TotalTokens == 19, legacy.InputTokens == 10,
            legacy.OutputTokens == 2, legacy.CacheReadTokens == 3,
            legacy.CacheWriteTokens == 4);

  records.Data[5].GenerationFinished.HasUsage = true;
  records.Data[5].GenerationFinished.Use =
      Usage{20, 5, 2, 1, 7, 40};

  SessionRecord name =
      BaseRecord(records.Data[0].SessionId, 11, 40,
                 SessionRecordKind::SessionName);
  name.CreatedAt = records.Data[7].CreatedAt + 100;
  name.SessionNameEntry.Name = SVL("  Named\n\tSession  ");
  Array_Push(&records, name);

  SessionMetadataBuildOptions options = {};
  options.NowMs = name.CreatedAt + 1000;
  SessionInfo info = {};
  CHECK(SessionResult_IsOk(SessionMetadata_Build(
      arena, records.Data, records.Count, &options, &info)));
  CHECK_ALL(StrView_EqCStr(info.DisplayTitle, "Named Session"),
            StrView_EqCStr(info.ExplicitName, "  Named\n\tSession  "),
            info.MessageCount == 3, info.UserMessageCount == 1,
            info.AssistantMessageCount == 1,
            info.ToolResultMessageCount == 1, info.ToolCallCount == 1,
            info.GenerationCount == 1, info.OperationCount == 1,
            info.TotalTokens == 40, info.InputTokens == 20,
            info.ReasoningTokens == 7,
            StrView_EqCStr(info.CurrentProvider, "anthropic"),
            StrView_EqCStr(info.CurrentModel, "claude-test"),
            info.LastActivityAt == records.Data[7].CreatedAt,
            info.StorageUpdatedAt == name.CreatedAt,
            info.ActiveTimeMs == 8,
            !info.Suspended && !info.Running);

  SessionRecord clear =
      BaseRecord(records.Data[0].SessionId, 12, 41,
                 SessionRecordKind::SessionName);
  clear.CreatedAt = name.CreatedAt + 1;
  clear.SessionNameEntry.Name = {};
  Array_Push(&records, clear);
  CHECK(SessionResult_IsOk(SessionMetadata_Build(
      arena, records.Data, records.Count, &options, &info)));
  CHECK_ALL(info.ExplicitName.Length == 0,
            StrView_EqCStr(info.DisplayTitle, "hello"),
            info.LastActivityAt == records.Data[7].CreatedAt);

  SessionMetadataBuildOptions suspendedOptions = {};
  CHECK(SessionResult_IsOk(SessionMetadata_Build(
      arena, records.Data, 4, &suspendedOptions, &info)));
  CHECK_ALL(info.Suspended, info.ActiveTimePartial, !info.Running,
            info.ActiveTimeMs == 2);
  SessionMetadataBuildOptions runningOptions = {};
  runningOptions.Busy = true;
  runningOptions.Running = true;
  runningOptions.NowMs = records.Data[1].CreatedAt + 250;
  CHECK(SessionResult_IsOk(SessionMetadata_Build(
      arena, records.Data, 4, &runningOptions, &info)));
  CHECK_ALL(info.Running, !info.Suspended, !info.ActiveTimePartial,
            info.ActiveTimeMs == 250);

  StrView flattened = SessionMetadata_FlattenDisplayText(
      arena, SVL(" \talpha\n\x01" "beta \xF0\x9F\x98\x80 tail"), 15);
  CHECK_ALL(
      StrView_EqCStr(flattened, "alpha beta \xF0\x9F\x98\x80"),
      flattened.Length == 15);
}

static SessionResult RepositoryCreate(SessionRepository* repository, Arena* arena,
                                      DurableId id, StrView projectKey,
                                      SessionLog* outLog, SessionInfo* outInfo) {
  SessionCreateOptions options = {};
  options.RequestedId = id;
  options.ProjectKey = projectKey;
  options.WorkingDirectory = SVL("C:\\work\\clank");
  options.Label = SVL("contract");
  return repository->Create(repository->Ctx, arena, &options, outLog, outInfo);
}

static void RunRepositoryContract(Arena* arena, SessionRepository* repository,
                                  StrView projectKey, u8 seed) {
  DurableId sessionId = TestId(seed);
  SessionLog writer = {};
  SessionInfo info = {};
  CHECK(SessionResult_IsOk(
      RepositoryCreate(repository, arena, sessionId, projectKey, &writer, &info)));
  CHECK_ALL(DurableId_Eq(info.SessionId, sessionId), info.Busy);

  SessionOpenOptions open = {};
  open.SessionId = sessionId;
  open.ProjectKey = projectKey;
  open.Writable = true;
  SessionLog secondWriter = {};
  SessionInfo secondInfo = {};
  CHECK(repository->Open(repository->Ctx, arena, &open, &secondWriter, &secondInfo)
            .Status == SessionStatus::Busy);

  Array<SessionRecord> initial = {};
  CHECK(SessionResult_IsOk(SessionLog_ReadAll(&writer, arena, &initial)));
  CHECK_ALL(initial.Count == 1,
            initial.Data[0].Kind == SessionRecordKind::SessionStarted);

  SessionRecord user = {};
  user.EntryId = TestId((u8)(seed + 1));
  user.Kind = SessionRecordKind::Message;
  user.MessageEntry.ParentId = initial.Data[0].EntryId;
  user.MessageEntry.Value = UserMessage(arena, "persist me");
  CHECK(SessionResult_IsOk(SessionLog_Append(&writer, &user)));
  CHECK_ALL(user.Seq == 2, DurableId_Eq(user.SessionId, sessionId),
            user.EntryClass == SessionEntryClass::Session);

  SessionRecord invalid = {};
  invalid.EntryId = TestId((u8)(seed + 2));
  invalid.Kind = SessionRecordKind::Message;
  invalid.MessageEntry.ParentId = TestId(99);
  invalid.MessageEntry.Value = UserMessage(arena, "bad parent");
  CHECK(SessionLog_Append(&writer, &invalid).Status ==
        SessionStatus::InvalidArgument);

  Array<SessionRecord> afterInvalid = {};
  CHECK(SessionResult_IsOk(SessionLog_ReadAll(&writer, arena, &afterInvalid)));
  CHECK(afterInvalid.Count == 2);

  SessionRecord name = {};
  name.EntryId = TestId((u8)(seed + 3));
  name.Kind = SessionRecordKind::SessionName;
  name.SessionNameEntry.Name = SVL("renamed");
  CHECK(SessionResult_IsOk(SessionLog_Append(&writer, &name)));
  SessionLog_Close(&writer);

  SessionInfo inspected = {};
  CHECK(SessionResult_IsOk(repository->Resolve(
      repository->Ctx, arena, sessionId, &inspected)));
  CHECK_ALL(StrView_EqCStr(inspected.ExplicitName, "renamed"),
            StrView_EqCStr(inspected.DisplayTitle, "renamed"),
            StrView_EqCStr(inspected.Label, "contract"));

  open.Writable = false;
  SessionLog reader = {};
  CHECK(SessionResult_IsOk(
      repository->Open(repository->Ctx, arena, &open, &reader, &info)));
  Array<SessionRecord> recovered = {};
  CHECK(SessionResult_IsOk(SessionLog_ReadAll(&reader, arena, &recovered)));
  CHECK(recovered.Count == 3);
  DurableId forkHead = recovered.Data[1].EntryId;
  SessionLog_Close(&reader);

  SessionForkOptions fork = {};
  fork.SourceSessionId = sessionId;
  fork.SourceHeadId = forkHead;
  fork.RequestedTargetId = TestId((u8)(seed + 10));
  fork.ProjectKey = open.ProjectKey;
  fork.WorkingDirectory = SVL("C:\\work\\clank");
  fork.Label = SVL("forked");
  SessionLog forkLog = {};
  SessionInfo forkInfo = {};
  CHECK(SessionResult_IsOk(
      repository->Fork(repository->Ctx, arena, &fork, &forkLog, &forkInfo)));
  Array<SessionRecord> forkRecords = {};
  CHECK(SessionResult_IsOk(SessionLog_ReadAll(&forkLog, arena, &forkRecords)));
  CHECK_ALL(forkRecords.Count == 2,
            forkRecords.Data[0].Kind == SessionRecordKind::SessionStarted,
            forkRecords.Data[1].Kind == SessionRecordKind::Message,
            !DurableId_Eq(forkRecords.Data[1].EntryId, forkHead),
            DurableId_Eq(forkRecords.Data[1].MessageEntry.ParentId,
                         forkRecords.Data[0].EntryId),
            DurableId_Eq(forkRecords.Data[0].SessionStarted.ForkedFromSessionId,
                         sessionId),
            DurableId_Eq(forkRecords.Data[0].SessionStarted.ForkedFromEntryId,
                         forkHead),
            DurableId_Eq(forkInfo.ForkedFromSessionId, sessionId),
            DurableId_Eq(forkInfo.ForkedFromEntryId, forkHead),
            StrView_EqCStr(forkInfo.DisplayTitle, "persist me"));
  SessionLog_Close(&forkLog);

  Array<SessionInfo> listed = {};
  SessionListOptions list = {};
  list.Scope = SessionListScope::CurrentProject;
  list.ProjectKey = open.ProjectKey;
  CHECK(SessionResult_IsOk(
      repository->List(repository->Ctx, arena, &list, &listed)));
  CHECK(listed.Count == 2);
  SessionInfo resolved = {};
  CHECK(SessionResult_IsOk(repository->Resolve(
      repository->Ctx, arena, sessionId, &resolved)));
  CHECK(StrView_Eq(resolved.ProjectKey, open.ProjectKey));
}

static void TestMemoryRepository(Arena* arena) {
  MemoryRepository memory = {};
  MemoryRepository_Init(&memory);
  SessionRepository repository = MemoryRepository_AsRepository(&memory);
  RunRepositoryContract(arena, &repository, SVL("clank-memory-contract"), 70);
  repository.Close(repository.Ctx);

  MemoryRepository_Init(&memory);
  repository = MemoryRepository_AsRepository(&memory);

  DurableId sessionId = TestId(90);
  StrView projectKey = SVL("clank-memory-failure");
  SessionLog writer = {};
  SessionInfo info = {};
  CHECK(SessionResult_IsOk(RepositoryCreate(
      &repository, arena, sessionId, projectKey, &writer, &info)));
  Array<SessionRecord> records = {};
  CHECK(SessionResult_IsOk(SessionLog_ReadAll(&writer, arena, &records)));
  MemoryRepository_FailAfterAppends(&memory, 0);
  SessionRecord model = {};
  model.EntryId = TestId(91);
  model.Kind = SessionRecordKind::Model;
  model.ModelEntry.ParentId = records.Data[0].EntryId;
  model.ModelEntry.Provider = SVL("anthropic");
  model.ModelEntry.Model = SVL("claude-test");
  CHECK(SessionLog_Append(&writer, &model).Status == SessionStatus::IoError);
  CHECK(SessionLog_Append(&writer, &model).Status == SessionStatus::Faulted);
  SessionLog_Close(&writer);

  MemoryRepository_ClearFailure(&memory);
  SessionOpenOptions open = {};
  open.SessionId = sessionId;
  open.ProjectKey = projectKey;
  SessionLog reader = {};
  CHECK(SessionResult_IsOk(
      repository.Open(repository.Ctx, arena, &open, &reader, &info)));
  CHECK(SessionResult_IsOk(SessionLog_ReadAll(&reader, arena, &records)));
  CHECK(records.Count == 1);
  SessionLog_Close(&reader);
  repository.Close(repository.Ctx);

  MemoryRepository_Init(&memory);
  repository = MemoryRepository_AsRepository(&memory);

  DurableId firstId = TestId(92);
  DurableId secondId = TestId(93);
  SessionLog firstLog = {};
  SessionLog secondLog = {};
  SessionInfo firstInfo = {};
  SessionInfo secondInfo = {};
  CHECK(SessionResult_IsOk(RepositoryCreate(
      &repository, arena, firstId, SVL("memory-project-a"), &firstLog,
      &firstInfo)));
  CHECK(SessionResult_IsOk(RepositoryCreate(
      &repository, arena, secondId, SVL("memory-project-b"), &secondLog,
      &secondInfo)));
  Array<SessionRecord> firstRoot = {};
  Array<SessionRecord> secondRoot = {};
  CHECK(SessionResult_IsOk(
      SessionLog_ReadAll(&firstLog, arena, &firstRoot)));
  CHECK(SessionResult_IsOk(
      SessionLog_ReadAll(&secondLog, arena, &secondRoot)));
  SessionRecord firstMessage = {};
  firstMessage.EntryId = TestId(94);
  firstMessage.Kind = SessionRecordKind::Message;
  firstMessage.CreatedAt = firstRoot.Data[0].CreatedAt + 1000;
  firstMessage.MessageEntry.ParentId = firstRoot.Data[0].EntryId;
  firstMessage.MessageEntry.Value = UserMessage(arena, "first project");
  SessionRecord secondMessage = {};
  secondMessage.EntryId = TestId(95);
  secondMessage.Kind = SessionRecordKind::Message;
  secondMessage.CreatedAt = secondRoot.Data[0].CreatedAt + 2000;
  secondMessage.MessageEntry.ParentId = secondRoot.Data[0].EntryId;
  secondMessage.MessageEntry.Value = UserMessage(arena, "second project");
  CHECK(SessionResult_IsOk(
      SessionLog_Append(&firstLog, &firstMessage)));
  CHECK(SessionResult_IsOk(
      SessionLog_Append(&secondLog, &secondMessage)));
  SessionRecord firstName = {};
  firstName.EntryId = TestId(96);
  firstName.Kind = SessionRecordKind::SessionName;
  firstName.SessionNameEntry.Name = SVL("named first");
  CHECK(SessionResult_IsOk(SessionLog_Append(&firstLog, &firstName)));
  SessionLog_Close(&firstLog);
  SessionLog_Close(&secondLog);

  SessionListOptions currentList = {};
  currentList.Scope = SessionListScope::CurrentProject;
  currentList.ProjectKey = SVL("memory-project-a");
  Array<SessionInfo> currentItems = {};
  CHECK(SessionResult_IsOk(repository.List(
      repository.Ctx, arena, &currentList, &currentItems)));
  CHECK_ALL(currentItems.Count == 1,
            StrView_EqCStr(currentItems.Data[0].DisplayTitle,
                           "named first"));
  SessionListOptions allList = {};
  allList.Scope = SessionListScope::AllProjects;
  Array<SessionInfo> allItems = {};
  CHECK(SessionResult_IsOk(repository.List(
      repository.Ctx, arena, &allList, &allItems)));
  CHECK_ALL(allItems.Count == 2,
            DurableId_Eq(allItems.Data[0].SessionId, secondId),
            allItems.Data[0].LastActivityAt >
                allItems.Data[1].LastActivityAt);
  SessionInfo globallyResolved = {};
  CHECK(SessionResult_IsOk(repository.Resolve(
      repository.Ctx, arena, secondId, &globallyResolved)));
  CHECK(StrView_EqCStr(globallyResolved.ProjectKey, "memory-project-b"));
  SessionOpenOptions clearOpen = {};
  clearOpen.SessionId = firstId;
  clearOpen.ProjectKey = SVL("memory-project-a");
  clearOpen.Writable = true;
  SessionLog clearLog = {};
  CHECK(SessionResult_IsOk(repository.Open(
      repository.Ctx, arena, &clearOpen, &clearLog, &globallyResolved)));
  SessionRecord clearName = {};
  clearName.EntryId = TestId(97);
  clearName.Kind = SessionRecordKind::SessionName;
  CHECK(SessionResult_IsOk(SessionLog_Append(&clearLog, &clearName)));
  SessionLog_Close(&clearLog);
  CHECK(SessionResult_IsOk(repository.Resolve(
      repository.Ctx, arena, firstId, &globallyResolved)));
  CHECK_ALL(globallyResolved.ExplicitName.Length == 0,
            StrView_EqCStr(globallyResolved.DisplayTitle,
                           "first project"));

  repository.Close(repository.Ctx);
}

static StrView JsonlArtifactPath(Arena* arena, StrView root, StrView project,
                                 DurableId sessionId, const char* suffix) {
  StrView sessions = Fs_Join(arena, root, SVL("sessions"));
  StrView version = Fs_Join(arena, sessions, SVL("v1"));
  StrView dir = Fs_Join(arena, version, project);
  StrBuf file = StrBuf_Make(arena, DURABLE_ID_CHAR_COUNT + 8);
  StrBuf_Append(&file, DurableId_View(&sessionId));
  StrBuf_AppendCStr(&file, suffix);
  return Fs_Join(arena, dir, StrBuf_View(&file));
}

static void RemovePath(Arena* arena, StrView path) {
  StrBuf native = StrBuf_Make(arena, path.Length + 1);
  StrBuf_Append(&native, path);
  remove(StrBuf_CStr(&native));
}

static void RemoveJsonlArtifacts(Arena* arena, StrView root, StrView project,
                                 DurableId sessionId) {
  RemovePath(arena, JsonlArtifactPath(arena, root, project, sessionId, ".jsonl"));
  RemovePath(arena, JsonlArtifactPath(arena, root, project, sessionId, ".lock"));
}

static void RemoveJsonlDirectories(Arena* arena, StrView root, StrView project) {
  StrView sessions = Fs_Join(arena, root, SVL("sessions"));
  StrView version = Fs_Join(arena, sessions, SVL("v1"));
  StrView dir = Fs_Join(arena, version, project);
  StrBuf native = StrBuf_Make(arena, dir.Length + 1);
  StrBuf_Append(&native, dir);
  SESSION_RMDIR(StrBuf_CStr(&native));
}

static void AppendRaw(Arena* arena, StrView path, const char* bytes) {
  StrBuf native = StrBuf_Make(arena, path.Length + 1);
  StrBuf_Append(&native, path);
  FILE* file = fopen(StrBuf_CStr(&native), "ab");
  if (!file) return;
  fwrite(bytes, 1, strlen(bytes), file);
  fclose(file);
}

static bool ChildSeesLockBusy(Arena* arena, StrView lockPath) {
  StrBuf command = StrBuf_Make(arena, lockPath.Length + 256);
#if defined(_WIN32)
  StrBuf_AppendCStr(&command, "call ");
#endif
  StrBuf_AppendChar(&command, '"');
  StrBuf_AppendCStr(&command, g_test_executable);
  StrBuf_AppendCStr(&command, "\" --expect-lock-busy \"");
  StrBuf_Append(&command, lockPath);
  StrBuf_AppendChar(&command, '"');
  return system(StrBuf_CStr(&command)) == 0;
}

static void TestJsonlRepository(Arena* arena) {
  StrView root = SVL("clank_jsonl_test");
  StrView contractProject = SVL("clank-jsonl-contract");
  StrView repairProject = SVL("clank-jsonl-repair");
  StrView corruptProject = SVL("clank-jsonl-corrupt");
  StrView harnessProject = SVL("clank-jsonl-harness");
  StrView scopeProjectA = SVL("clank-jsonl-scope-a");
  StrView scopeProjectB = SVL("clank-jsonl-scope-b");
  DurableId contractId = TestId(120);
  DurableId forkId = TestId(130);
  DurableId repairId = TestId(140);
  DurableId corruptId = TestId(150);
  DurableId harnessId = TestId(160);
  DurableId harnessForkId = TestId(170);
  DurableId scopeIdA = TestId(180);
  DurableId scopeIdB = TestId(190);
  RemoveJsonlArtifacts(arena, root, contractProject, contractId);
  RemoveJsonlArtifacts(arena, root, contractProject, forkId);
  RemoveJsonlArtifacts(arena, root, repairProject, repairId);
  RemoveJsonlArtifacts(arena, root, corruptProject, corruptId);
  RemoveJsonlArtifacts(arena, root, harnessProject, harnessId);
  RemoveJsonlArtifacts(arena, root, harnessProject, harnessForkId);
  RemoveJsonlArtifacts(arena, root, scopeProjectA, scopeIdA);
  RemoveJsonlArtifacts(arena, root, scopeProjectB, scopeIdB);

  JsonlRepository jsonl = {};
  JsonlRepository_Init(&jsonl, root);
  SessionRepository repository = JsonlRepository_AsRepository(&jsonl);
  RunRepositoryContract(arena, &repository, contractProject, 120);
  RemoveJsonlArtifacts(arena, root, contractProject, contractId);
  RemoveJsonlArtifacts(arena, root, contractProject, forkId);

  SessionLog scopeLogA = {};
  SessionLog scopeLogB = {};
  SessionInfo scopeInfoA = {};
  SessionInfo scopeInfoB = {};
  CHECK(SessionResult_IsOk(RepositoryCreate(
      &repository, arena, scopeIdA, scopeProjectA, &scopeLogA,
      &scopeInfoA)));
  SessionLog duplicateScopeLog = {};
  SessionInfo duplicateScopeInfo = {};
  CHECK(RepositoryCreate(&repository, arena, scopeIdA, scopeProjectB,
                         &duplicateScopeLog, &duplicateScopeInfo)
            .Status == SessionStatus::Conflict);
  CHECK(SessionResult_IsOk(RepositoryCreate(
      &repository, arena, scopeIdB, scopeProjectB, &scopeLogB,
      &scopeInfoB)));
  SessionLog_Close(&scopeLogA);
  SessionLog_Close(&scopeLogB);
  SessionListOptions allProjects = {};
  allProjects.Scope = SessionListScope::AllProjects;
  Array<SessionInfo> allProjectItems = {};
  CHECK(SessionResult_IsOk(repository.List(
      repository.Ctx, arena, &allProjects, &allProjectItems)));
  CHECK(allProjectItems.Count == 2);
  SessionInfo scopeResolved = {};
  CHECK(SessionResult_IsOk(repository.Resolve(
      repository.Ctx, arena, scopeIdB, &scopeResolved)));
  CHECK(StrView_Eq(scopeResolved.ProjectKey, scopeProjectB));

  SessionLog harnessLog = {};
  SessionInfo harnessInfo = {};
  CHECK(SessionResult_IsOk(RepositoryCreate(
      &repository, arena, harnessId, harnessProject, &harnessLog,
      &harnessInfo)));
  Array<SessionRecord> harnessRoot = {};
  CHECK(SessionResult_IsOk(
      SessionLog_ReadAll(&harnessLog, arena, &harnessRoot)));
  SessionRecord harnessMessage = {};
  harnessMessage.EntryId = TestId(161);
  harnessMessage.Kind = SessionRecordKind::Message;
  harnessMessage.MessageEntry.ParentId = harnessRoot.Data[0].EntryId;
  harnessMessage.MessageEntry.Value =
      UserMessage(arena, "fork through harness");
  CHECK(SessionResult_IsOk(SessionLog_Append(&harnessLog, &harnessMessage)));
  AgentHarnessConfig harnessConfig = {};
  AgentHarness harness = {};
  CHECK(SessionResult_IsOk(
      AgentHarness_Init(&harness, harnessLog, &harnessConfig)));
  SessionLog harnessFork = {};
  SessionInfo harnessForkInfo = {};
  CHECK(SessionResult_IsOk(AgentHarness_Fork(
      arena, &harness, &repository, harnessMessage.EntryId, harnessForkId,
      SVL("harness fork"), &harnessFork, &harnessForkInfo)));
  Array<SessionRecord> harnessForkRecords = {};
  CHECK(SessionResult_IsOk(
      SessionLog_ReadAll(&harnessFork, arena, &harnessForkRecords)));
  CHECK_ALL(
      harnessForkRecords.Count == 2,
      harnessForkRecords.Data[1].Kind == SessionRecordKind::Message,
      DurableId_Eq(
          harnessForkRecords.Data[0].SessionStarted.ForkedFromSessionId,
          harnessId),
      DurableId_Eq(
          harnessForkRecords.Data[0].SessionStarted.ForkedFromEntryId,
          harnessMessage.EntryId));
  SessionLog_Close(&harnessFork);
  CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));

  SessionLog writer = {};
  SessionInfo info = {};
  CHECK(SessionResult_IsOk(RepositoryCreate(
      &repository, arena, repairId, repairProject, &writer, &info)));
  CHECK(ChildSeesLockBusy(
      arena, JsonlArtifactPath(arena, root, repairProject, repairId, ".lock")));
  Array<SessionRecord> rootRecords = {};
  CHECK(SessionResult_IsOk(SessionLog_ReadAll(&writer, arena, &rootRecords)));
  SessionRecord user = {};
  user.EntryId = TestId(141);
  user.Kind = SessionRecordKind::Message;
  user.MessageEntry.ParentId = rootRecords.Data[0].EntryId;
  user.MessageEntry.Value = UserMessage(arena, "before crash");
  CHECK(SessionResult_IsOk(SessionLog_Append(&writer, &user)));
  SessionLog_Close(&writer);

  StrView repairPath =
      JsonlArtifactPath(arena, root, repairProject, repairId, ".jsonl");
  AppendRaw(arena, repairPath, "{\"torn\":");
  SessionOpenOptions open = {};
  open.SessionId = repairId;
  open.ProjectKey = repairProject;
  open.Writable = true;
  CHECK(SessionResult_IsOk(
      repository.Open(repository.Ctx, arena, &open, &writer, &info)));
  Array<SessionRecord> repaired = {};
  CHECK(SessionResult_IsOk(SessionLog_ReadAll(&writer, arena, &repaired)));
  CHECK(repaired.Count == 2);
  SessionLog_Close(&writer);

  StrView repairedBytes = {};
  CHECK(ConfigFile_ReadAll(arena, repairPath, &repairedBytes) ==
        ConfigFileReadResult::Success);
  CHECK_ALL(repairedBytes.Length > 0,
            repairedBytes.Data[repairedBytes.Length - 1] == '\n',
            !StrView_Contains(repairedBytes, "\"torn\""));

  CHECK(SessionResult_IsOk(RepositoryCreate(
      &repository, arena, corruptId, corruptProject, &writer, &info)));
  SessionLog_Close(&writer);
  StrView corruptPath =
      JsonlArtifactPath(arena, root, corruptProject, corruptId, ".jsonl");
  AppendRaw(arena, corruptPath, "not-json\n");
  SessionListOptions corruptList = {};
  corruptList.Scope = SessionListScope::CurrentProject;
  corruptList.ProjectKey = corruptProject;
  u32 skippedCount = 0;
  corruptList.SkippedCount = &skippedCount;
  Array<SessionInfo> corruptItems = {};
  CHECK(SessionResult_IsOk(repository.List(
      repository.Ctx, arena, &corruptList, &corruptItems)));
  CHECK_ALL(corruptItems.Count == 0, skippedCount == 1);
  SessionInfo corruptInfo = {};
  CHECK(repository.Resolve(repository.Ctx, arena, corruptId, &corruptInfo)
            .Status == SessionStatus::Corrupt);
  open.SessionId = corruptId;
  open.ProjectKey = corruptProject;
  open.Writable = true;
  CHECK(repository.Open(repository.Ctx, arena, &open, &writer, &info).Status ==
        SessionStatus::Corrupt);

  repository.Close(repository.Ctx);

  RemoveJsonlArtifacts(arena, root, contractProject, contractId);
  RemoveJsonlArtifacts(arena, root, contractProject, forkId);
  RemoveJsonlArtifacts(arena, root, repairProject, repairId);
  RemoveJsonlArtifacts(arena, root, corruptProject, corruptId);
  RemoveJsonlArtifacts(arena, root, harnessProject, harnessId);
  RemoveJsonlArtifacts(arena, root, harnessProject, harnessForkId);
  RemoveJsonlArtifacts(arena, root, scopeProjectA, scopeIdA);
  RemoveJsonlArtifacts(arena, root, scopeProjectB, scopeIdB);
  RemoveJsonlDirectories(arena, root, contractProject);
  RemoveJsonlDirectories(arena, root, repairProject);
  RemoveJsonlDirectories(arena, root, corruptProject);
  RemoveJsonlDirectories(arena, root, harnessProject);
  RemoveJsonlDirectories(arena, root, scopeProjectA);
  RemoveJsonlDirectories(arena, root, scopeProjectB);
  StrView sessions = Fs_Join(arena, root, SVL("sessions"));
  StrView version = Fs_Join(arena, sessions, SVL("v1"));
  StrBuf versionNative = StrBuf_Make(arena, version.Length + 1);
  StrBuf_Append(&versionNative, version);
  SESSION_RMDIR(StrBuf_CStr(&versionNative));
  StrBuf sessionsNative = StrBuf_Make(arena, sessions.Length + 1);
  StrBuf_Append(&sessionsNative, sessions);
  SESSION_RMDIR(StrBuf_CStr(&sessionsNative));
  StrBuf rootNative = StrBuf_Make(arena, root.Length + 1);
  StrBuf_Append(&rootNative, root);
  SESSION_RMDIR(StrBuf_CStr(&rootNative));
}

void TestSession() {
  Arena arena = ArenaCreate();
  TestDurableIds();
  TestRecordCodec(&arena);
  TestRecordValidation(&arena);
  TestReducer(&arena);
  TestMetadata(&arena);
  TestMemoryRepository(&arena);
  TestJsonlRepository(&arena);
  ArenaRelease(&arena);
}
