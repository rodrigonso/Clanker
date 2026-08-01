#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent/harness.h"
#include "base/arena.h"
#include "base/config_file.h"
#include "check.h"
#include "session/entry.h"
#include "session/jsonl_repository.h"
#include "session/memory_repository.h"
#include "session/metadata.h"
#include "session/reducer.h"
#include "session/replay.h"
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

static SessionRecord BaseRecord(DurableId sessionId, u64 seq, u8 idSeed, SessionRecordKind kind) {
  SessionRecord record = {};
  record.SchemaVersion = SESSION_SCHEMA_VERSION;
  record.SessionId = sessionId;
  record.Seq = seq;
  record.EntryId = TestId(idSeed);
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
  message.Content.Data[1].Signature = SVL("fc-item-1");
  return message;
}

static Message AbortedAssistantMessage(Arena* arena, u64 timestamp) {
  Message message = Message_AssistantEmpty(arena);
  message.Timestamp = timestamp;
  message.Stop = StopReason::Aborted;
  return message;
}

// ---------------------------------------------------------------------------
// v2 record codec: schema-version gating, per-kind payload round trips.
// ---------------------------------------------------------------------------

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

  SessionRecord message = BaseRecord(sessionId, 2, 20, SessionRecordKind::Message);
  message.MessageEntry.ParentId = TestId(1);
  message.MessageEntry.Value = AssistantToolMessage(arena);
  StrBuf encoded = StrBuf_Make(arena, 512);
  CHECK(SessionResult_IsOk(SessionRecord_Encode(&encoded, &message)));

  SessionRecord decoded = {};
  CHECK(SessionResult_IsOk(SessionRecord_Decode(arena, StrBuf_View(&encoded), &decoded)));
  CHECK_ALL(decoded.Kind == SessionRecordKind::Message, decoded.Seq == message.Seq,
            DurableId_Eq(decoded.SessionId, message.SessionId),
            DurableId_Eq(decoded.MessageEntry.ParentId, message.MessageEntry.ParentId),
            decoded.MessageEntry.Value.Kind == Role::Assistant,
            decoded.MessageEntry.Value.Use.Input == 10,
            decoded.MessageEntry.Value.Content.Count == 2,
            StrView_EqCStr(decoded.MessageEntry.Value.Content.Data[1].ToolName, "read"),
            StrView_EqCStr(decoded.MessageEntry.Value.Content.Data[1].Signature,
                           "fc-item-1"),
            StrView_EqCStr(decoded.MessageEntry.Value.Model, "claude-test"));

  message.MessageEntry.Value.Content.Data[1].Signature = {};
  StrBuf legacyToolCall = StrBuf_Make(arena, 512);
  CHECK(SessionResult_IsOk(
      SessionRecord_Encode(&legacyToolCall, &message)));
  CHECK(!StrView_Contains(StrBuf_View(&legacyToolCall), "\"signature\":\"fc-item-1\""));
  CHECK(SessionResult_IsOk(SessionRecord_Decode(
      arena, StrBuf_View(&legacyToolCall), &decoded)));
  CHECK_ALL(
      decoded.MessageEntry.Value.Content.Count == 2,
      decoded.MessageEntry.Value.Content.Data[1].Signature.Length == 0);
  message.MessageEntry.Value.Content.Data[1].Signature = SVL("fc-item-1");

  message.MessageEntry.Value.Content.Data[1].ToolArgsJson = SVL("not-json");
  StrBuf invalidArguments = StrBuf_Make(arena, 512);
  CHECK(SessionResult_IsOk(SessionRecord_Encode(&invalidArguments, &message)));
  CHECK(SessionResult_IsOk(
      SessionRecord_Decode(arena, StrBuf_View(&invalidArguments), &decoded)));
  CHECK(StrView_EqCStr(
      decoded.MessageEntry.Value.Content.Data[1].ToolArgsJson, "not-json"));
  message.MessageEntry.Value.Content.Data[1].ToolArgsJson =
      SVL("{\"path\":\"a.txt\"}");

  StrBuf trailing = StrBuf_Make(arena, encoded.Length + 8);
  StrBuf_Append(&trailing, StrBuf_View(&encoded));
  StrBuf_AppendCStr(&trailing, " trailing");
  CHECK(SessionRecord_Decode(arena, StrBuf_View(&trailing), &decoded).Status ==
        SessionStatus::Corrupt);

  const char* unknownType =
      "{\"schemaVersion\":2,\"sessionId\":\"AQIDBAUGBwgJCgsMDQ4PEA\",\"seq\":1,"
      "\"entryId\":\"AgMEBQYHCAkKCwwNDg8QEQ\",\"type\":\"future_type\","
      "\"createdAt\":1,\"payload\":{}}";
  CHECK(SessionRecord_Decode(arena, SV(unknownType), &decoded).Status == SessionStatus::Corrupt);

  const char* futureVersion =
      "{\"schemaVersion\":3,\"sessionId\":\"AQIDBAUGBwgJCgsMDQ4PEA\",\"seq\":1,"
      "\"entryId\":\"AgMEBQYHCAkKCwwNDg8QEQ\",\"type\":\"session_started\","
      "\"createdAt\":1,\"payload\":{}}";
  CHECK(SessionRecord_Decode(arena, SV(futureVersion), &decoded).Status ==
        SessionStatus::UnsupportedVersion);

  const char* pastVersion =
      "{\"schemaVersion\":1,\"sessionId\":\"AQIDBAUGBwgJCgsMDQ4PEA\",\"seq\":1,"
      "\"entryId\":\"AgMEBQYHCAkKCwwNDg8QEQ\",\"type\":\"session_started\","
      "\"createdAt\":1,\"payload\":{}}";
  CHECK(SessionRecord_Decode(arena, SV(pastVersion), &decoded).Status == SessionStatus::Corrupt);

  SessionRecord compaction = BaseRecord(sessionId, 6, 30, SessionRecordKind::Compaction);
  compaction.CompactionEntry.ParentId = TestId(1);
  compaction.CompactionEntry.BaseEntryId = TestId(1);
  compaction.CompactionEntry.ThroughEntryId = TestId(1);
  compaction.CompactionEntry.SourceHeadId = TestId(1);
  compaction.CompactionEntry.Summary = UserMessage(arena, "summary text");
  compaction.CompactionEntry.HasUsage = true;
  compaction.CompactionEntry.Use = Usage{11, 12, 13, 14, 15, 65};
  StrBuf compactionJson = StrBuf_Make(arena, 512);
  CHECK(SessionResult_IsOk(SessionRecord_Encode(&compactionJson, &compaction)));
  CHECK(StrView_Contains(StrBuf_View(&compactionJson), "\"usage\""));
  CHECK(SessionResult_IsOk(SessionRecord_Decode(arena, StrBuf_View(&compactionJson), &decoded)));
  CHECK_ALL(decoded.CompactionEntry.HasUsage, decoded.CompactionEntry.Use.TotalTokens == 65,
            StrView_EqCStr(decoded.CompactionEntry.Summary.Content.Data[0].Text, "summary text"));

  compaction.CompactionEntry.HasUsage = false;
  StrBuf noUsageJson = StrBuf_Make(arena, 512);
  CHECK(SessionResult_IsOk(SessionRecord_Encode(&noUsageJson, &compaction)));
  CHECK(!StrView_Contains(StrBuf_View(&noUsageJson), "\"usage\""));
  CHECK(SessionResult_IsOk(SessionRecord_Decode(arena, StrBuf_View(&noUsageJson), &decoded)));
  CHECK(!decoded.CompactionEntry.HasUsage);

  SessionRecord named = BaseRecord(sessionId, 7, 40, SessionRecordKind::SessionName);
  named.SessionNameEntry.Name = {};
  StrBuf namedJson = StrBuf_Make(arena, 256);
  CHECK(SessionResult_IsOk(SessionRecord_Encode(&namedJson, &named)));
  CHECK(SessionResult_IsOk(SessionRecord_Decode(arena, StrBuf_View(&namedJson), &decoded)));
  CHECK_ALL(decoded.Kind == SessionRecordKind::SessionName, decoded.SessionNameEntry.Name.Length == 0);

  SessionRecord tools = BaseRecord(sessionId, 8, 50, SessionRecordKind::Tools);
  tools.ToolsEntry.ParentId = TestId(1);
  StrView names[] = {SVL("read"), SVL("write")};
  tools.ToolsEntry.ToolNames = Array<StrView>{names, 2, 2, nullptr};
  StrBuf toolsJson = StrBuf_Make(arena, 256);
  CHECK(SessionResult_IsOk(SessionRecord_Encode(&toolsJson, &tools)));
  CHECK(SessionResult_IsOk(SessionRecord_Decode(arena, StrBuf_View(&toolsJson), &decoded)));
  CHECK_ALL(decoded.ToolsEntry.ToolNames.Count == 2,
            StrView_EqCStr(decoded.ToolsEntry.ToolNames.Data[0], "read"),
            StrView_EqCStr(decoded.ToolsEntry.ToolNames.Data[1], "write"));

  SessionRecord leaf = BaseRecord(sessionId, 9, 60, SessionRecordKind::Leaf);
  leaf.LeafEntry.LeafId = TestId(1);
  StrBuf leafJson = StrBuf_Make(arena, 256);
  CHECK(SessionResult_IsOk(SessionRecord_Encode(&leafJson, &leaf)));
  CHECK(SessionResult_IsOk(SessionRecord_Decode(arena, StrBuf_View(&leafJson), &decoded)));
  CHECK_ALL(decoded.Kind == SessionRecordKind::Leaf, DurableId_Eq(decoded.LeafEntry.LeafId, TestId(1)));
}

// Explicit structural clone must not depend on the source's arena: releasing
// it after cloning must not corrupt (or crash reading) the clone.
static void TestRecordClone() {
  Arena scratch = ArenaCreate();
  Arena dest = ArenaCreate();
  SessionRecord source = BaseRecord(TestId(1), 2, 5, SessionRecordKind::Message);
  source.MessageEntry.ParentId = TestId(1);
  source.MessageEntry.Value = UserMessage(&scratch, "clone me");

  SessionRecord clone = {};
  CHECK(SessionResult_IsOk(SessionRecord_Clone(&dest, &source, &clone)));
  ArenaRelease(&scratch);

  CHECK_ALL(DurableId_Eq(clone.EntryId, source.EntryId), clone.MessageEntry.Value.Kind == Role::User,
            StrView_EqCStr(clone.MessageEntry.Value.Content.Data[0].Text, "clone me"));
  ArenaRelease(&dest);
}

static Array<SessionRecord> BuildValidTrace(Arena* arena) {
  DurableId sessionId = TestId(1);
  DurableId rootId = TestId(2);
  DurableId userId = TestId(4);
  DurableId assistantId = TestId(6);
  DurableId toolResultId = TestId(8);

  Array<SessionRecord> records = Array_Make<SessionRecord>(arena, 4);

  SessionRecord started = BaseRecord(sessionId, 1, 2, SessionRecordKind::SessionStarted);
  started.EntryId = rootId;
  started.SessionStarted.WorkingDirectory = SVL("C:\\work\\clank");
  started.SessionStarted.ProjectKey = SVL("clank-1234");
  started.SessionStarted.Label = SVL("test");
  Array_Push(&records, started);

  SessionRecord user = BaseRecord(sessionId, 2, 4, SessionRecordKind::Message);
  user.EntryId = userId;
  user.MessageEntry.ParentId = rootId;
  user.MessageEntry.Value = UserMessage(arena, "hello");
  Array_Push(&records, user);

  SessionRecord assistant = BaseRecord(sessionId, 3, 6, SessionRecordKind::Message);
  assistant.EntryId = assistantId;
  assistant.MessageEntry.ParentId = userId;
  assistant.MessageEntry.Value = AssistantToolMessage(arena);
  Array_Push(&records, assistant);

  SessionRecord result = BaseRecord(sessionId, 4, 8, SessionRecordKind::Message);
  result.EntryId = toolResultId;
  result.MessageEntry.ParentId = assistantId;
  result.MessageEntry.Value =
      Message_ToolResult(arena, SVL("call-1"), SVL("read"), SVL("contents"), false);
  result.MessageEntry.Value.Timestamp = 1700000000002ull;
  Array_Push(&records, result);

  return records;
}

static void TestRecordValidation(Arena* arena) {
  Array<SessionRecord> records = BuildValidTrace(arena);
  SessionResult valid = SessionRecords_Validate(records.Data, records.Count);
  CHECK(SessionResult_IsOk(valid));

  DurableId savedParent = records.Data[2].MessageEntry.ParentId;
  records.Data[2].MessageEntry.ParentId = TestId(99);
  CHECK(SessionRecords_Validate(records.Data, records.Count).Status == SessionStatus::Corrupt);
  records.Data[2].MessageEntry.ParentId = savedParent;

  u64 savedSeq = records.Data[3].Seq;
  records.Data[3].Seq = 9;
  CHECK(SessionRecords_Validate(records.Data, records.Count).Status == SessionStatus::Corrupt);
  records.Data[3].Seq = savedSeq;

  DurableId savedId = records.Data[3].EntryId;
  records.Data[3].EntryId = records.Data[1].EntryId;
  CHECK(SessionRecords_Validate(records.Data, records.Count).Status == SessionStatus::Corrupt);
  records.Data[3].EntryId = savedId;

  SessionRecord badCompaction =
      BaseRecord(records.Data[0].SessionId, 5, 90, SessionRecordKind::Compaction);
  badCompaction.CompactionEntry.ParentId = records.Data[3].EntryId;
  badCompaction.CompactionEntry.BaseEntryId = records.Data[0].EntryId;
  badCompaction.CompactionEntry.ThroughEntryId = TestId(77);  // never appended
  badCompaction.CompactionEntry.SourceHeadId = records.Data[3].EntryId;
  badCompaction.CompactionEntry.Summary = UserMessage(arena, "summary");
  Array<SessionRecord> withBadCompaction = Array_Make<SessionRecord>(arena, 5);
  for (u32 i = 0; i < records.Count; i++) Array_Push(&withBadCompaction, records.Data[i]);
  Array_Push(&withBadCompaction, badCompaction);
  CHECK(SessionRecords_Validate(withBadCompaction.Data, withBadCompaction.Count).Status ==
        SessionStatus::Corrupt);

  CHECK(SessionRecords_Validate(records.Data + 1, records.Count - 1).Status ==
        SessionStatus::Corrupt);
}

static void TestReducer(Arena* arena) {
  Array<SessionRecord> records = BuildValidTrace(arena);
  SessionReduction reduction = {};
  SessionResult reduced = SessionReducer_Reduce(arena, records.Data, records.Count, &reduction);
  CHECK(SessionResult_IsOk(reduced));
  CHECK(DurableIdIndex_AllocationSize(262145) > ((usize)16 << 20));
  CHECK_ALL(reduction.HeadIsSet, DurableId_Eq(reduction.HeadId, records.Data[3].EntryId),
            reduction.RecordCount == 4);

  Array<Message> context = {};
  CHECK(SessionResult_IsOk(
      SessionReducer_Context(arena, &reduction, reduction.HeadId, &context)));
  CHECK_ALL(context.Count == 3, context.Data[0].Kind == Role::User,
            context.Data[1].Kind == Role::Assistant, context.Data[2].Kind == Role::ToolResult);

  // Appending onto the active head is validated in O(1) via the index --
  // never a rescan of the whole log -- then folds in O(1) too (ordinary,
  // non-Leaf record).
  SessionRecord model = {};
  model.SchemaVersion = SESSION_SCHEMA_VERSION;
  model.SessionId = records.Data[0].SessionId;
  model.Seq = 5;
  model.EntryId = TestId(50);
  model.Kind = SessionRecordKind::Model;
  model.ModelEntry.ParentId = reduction.HeadId;
  model.ModelEntry.Provider = SVL("anthropic");
  model.ModelEntry.Model = SVL("claude-test");
  CHECK(SessionResult_IsOk(SessionReducer_ValidateAppend(&reduction, &model)));
  Array_Push(&records, model);
  SessionReducer_Fold(&reduction, records.Data, records.Count);
  CHECK_ALL(DurableId_Eq(reduction.HeadId, model.EntryId),
            StrView_EqCStr(reduction.Provider, "anthropic"));

  // A duplicate entryId or a parent that was never appended is rejected
  // without rescanning history (index lookups only).
  SessionRecord duplicate = model;
  duplicate.Seq = 6;
  duplicate.EntryId = records.Data[1].EntryId;
  CHECK(SessionReducer_ValidateAppend(&reduction, &duplicate).Status ==
        SessionStatus::InvalidArgument);

  SessionRecord orphan = {};
  orphan.SchemaVersion = SESSION_SCHEMA_VERSION;
  orphan.SessionId = records.Data[0].SessionId;
  orphan.Seq = 6;
  orphan.EntryId = TestId(51);
  orphan.Kind = SessionRecordKind::Message;
  orphan.MessageEntry.ParentId = TestId(222);
  orphan.MessageEntry.Value = UserMessage(arena, "orphan");
  CHECK(SessionReducer_ValidateAppend(&reduction, &orphan).Status ==
        SessionStatus::InvalidArgument);

  // Same-batch references may target only tree nodes. Non-tree IDs still
  // participate in duplicate detection, but never become valid parents.
  SessionRecord pendingName =
      BaseRecord(records.Data[0].SessionId, 6, 52,
                 SessionRecordKind::SessionName);
  pendingName.SessionNameEntry.Name = SVL("pending name");
  SessionRecord childOfName =
      BaseRecord(records.Data[0].SessionId, 7, 53,
                 SessionRecordKind::Message);
  childOfName.MessageEntry.ParentId = pendingName.EntryId;
  childOfName.MessageEntry.Value = UserMessage(arena, "invalid child");
  CHECK(SessionReducer_ValidateAppend(&reduction, &childOfName, &pendingName,
                                      1)
            .Status == SessionStatus::InvalidArgument);
  SessionRecord duplicatePending = childOfName;
  duplicatePending.EntryId = pendingName.EntryId;
  duplicatePending.MessageEntry.ParentId = reduction.HeadId;
  CHECK(SessionReducer_ValidateAppend(&reduction, &duplicatePending,
                                      &pendingName, 1)
            .Status == SessionStatus::InvalidArgument);

  // Thinking/Tools chain onto the new head; checking out (Leaf) an older
  // entry recomputes inherited branch settings in O(branch), and clears them
  // once none of the ancestors set them.
  SessionRecord thinking = {};
  thinking.SchemaVersion = SESSION_SCHEMA_VERSION;
  thinking.SessionId = records.Data[0].SessionId;
  thinking.Seq = 6;
  thinking.EntryId = TestId(52);
  thinking.Kind = SessionRecordKind::Thinking;
  thinking.ThinkingEntry.ParentId = model.EntryId;
  thinking.ThinkingEntry.Level = SVL("high");
  Array_Push(&records, thinking);
  SessionReducer_Fold(&reduction, records.Data, records.Count);

  StrView toolNames[] = {SVL("read")};
  SessionRecord toolsEntry = {};
  toolsEntry.SchemaVersion = SESSION_SCHEMA_VERSION;
  toolsEntry.SessionId = records.Data[0].SessionId;
  toolsEntry.Seq = 7;
  toolsEntry.EntryId = TestId(53);
  toolsEntry.Kind = SessionRecordKind::Tools;
  toolsEntry.ToolsEntry.ParentId = thinking.EntryId;
  toolsEntry.ToolsEntry.ToolNames = Array<StrView>{toolNames, 1, 1, nullptr};
  Array_Push(&records, toolsEntry);
  SessionReducer_Fold(&reduction, records.Data, records.Count);
  CHECK_ALL(StrView_EqCStr(reduction.ThinkingLevel, "high"), reduction.ToolNames.Count == 1,
            StrView_EqCStr(reduction.ToolNames.Data[0], "read"));

  SessionRecord leaf = {};
  leaf.SchemaVersion = SESSION_SCHEMA_VERSION;
  leaf.SessionId = records.Data[0].SessionId;
  leaf.Seq = 8;
  leaf.EntryId = TestId(54);
  leaf.Kind = SessionRecordKind::Leaf;
  leaf.LeafEntry.LeafId = records.Data[3].EntryId;
  Array_Push(&records, leaf);
  SessionReducer_Fold(&reduction, records.Data, records.Count);
  CHECK_ALL(DurableId_Eq(reduction.HeadId, records.Data[3].EntryId),
            reduction.ThinkingLevel.Length == 0, reduction.ToolNames.Count == 0,
            reduction.Provider.Length == 0);

  SessionRecord duplicateLeafId =
      BaseRecord(records.Data[0].SessionId, 9, 55,
                 SessionRecordKind::SessionName);
  duplicateLeafId.EntryId = leaf.EntryId;
  duplicateLeafId.SessionNameEntry.Name = SVL("duplicate leaf ID");
  CHECK(SessionReducer_ValidateAppend(&reduction, &duplicateLeafId).Status ==
        SessionStatus::InvalidArgument);
  Array<SessionRecord> invalidHistory =
      Array_Make<SessionRecord>(arena, records.Count + 1);
  for (u32 i = 0; i < records.Count; i++)
    Array_Push(&invalidHistory, records.Data[i]);
  Array_Push(&invalidHistory, duplicateLeafId);
  CHECK(SessionRecords_Validate(invalidHistory.Data, invalidHistory.Count)
            .Status == SessionStatus::Corrupt);

  Array<const SessionRecord*> ancestry = {};
  CHECK(SessionResult_IsOk(
      SessionReducer_Ancestry(&reduction, arena, records.Data[3].EntryId, &ancestry)));
  CHECK(ancestry.Count == 4);
}

// ---------------------------------------------------------------------------
// Replay normalization (session/replay.h): orphan tool-call synthesis and
// omission of aborted/errored assistant turns. Transient/request-time only --
// exercised here directly against SessionReplay_TransformMessages, never
// touching a session log.
// ---------------------------------------------------------------------------

static void TestReplay(Arena* arena) {
  CHECK(StrView_EqCStr(SessionReplay_NoResultMessage(), "No result provided"));

  Message messages[7];
  messages[0] = UserMessage(arena, "first");

  messages[1] = Message_AssistantEmpty(arena);
  messages[1].Timestamp = 100;
  messages[1].Stop = StopReason::ToolUse;
  Array_Push(&messages[1].Content, Content_ToolCall(SVL("call-a"), SVL("read"), SVL("{}")));
  Array_Push(&messages[1].Content, Content_ToolCall(SVL("call-b"), SVL("write"), SVL("{}")));

  messages[2] = Message_ToolResult(arena, SVL("call-a"), SVL("read"), SVL("result-a"), false);
  messages[2].Timestamp = 150;
  // call-b never gets a result before the next user message: it is
  // interrupted and must be synthesized right before that next message.

  messages[3] = UserMessage(arena, "second");

  messages[4] = AbortedAssistantMessage(arena, 200);  // omitted entirely, including from pending

  messages[5] = UserMessage(arena, "third");

  messages[6] = Message_AssistantEmpty(arena);
  messages[6].Timestamp = 250;
  messages[6].Stop = StopReason::ToolUse;
  Array_Push(&messages[6].Content, Content_ToolCall(SVL("call-c"), SVL("shell"), SVL("{}")));
  // call-c is interrupted at end-of-array (process exit) and must be
  // synthesized as a trailing entry.

  Array<Message> transformed = SessionReplay_TransformMessages(arena, messages, 7);
  CHECK(transformed.Count == 8);
  CHECK_ALL(transformed.Data[0].Kind == Role::User,
            StrView_EqCStr(transformed.Data[0].Content.Data[0].Text, "first"),
            transformed.Data[1].Kind == Role::Assistant, transformed.Data[1].Content.Count == 2,
            transformed.Data[2].Kind == Role::ToolResult,
            StrView_EqCStr(transformed.Data[2].ToolCallId, "call-a"), !transformed.Data[2].IsError);

  const Message& syntheticB = transformed.Data[3];
  CHECK_ALL(syntheticB.Kind == Role::ToolResult, StrView_EqCStr(syntheticB.ToolCallId, "call-b"),
            StrView_EqCStr(syntheticB.ToolName, "write"), syntheticB.IsError,
            StrView_EqCStr(syntheticB.Content.Data[0].Text, "No result provided"));

  CHECK_ALL(transformed.Data[4].Kind == Role::User,
            StrView_EqCStr(transformed.Data[4].Content.Data[0].Text, "second"),
            transformed.Data[5].Kind == Role::User,
            StrView_EqCStr(transformed.Data[5].Content.Data[0].Text, "third"),
            transformed.Data[6].Kind == Role::Assistant, transformed.Data[6].Content.Count == 1);

  const Message& syntheticC = transformed.Data[7];
  CHECK_ALL(syntheticC.Kind == Role::ToolResult, StrView_EqCStr(syntheticC.ToolCallId, "call-c"),
            StrView_EqCStr(syntheticC.ToolName, "shell"), syntheticC.IsError,
            StrView_EqCStr(syntheticC.Content.Data[0].Text, "No result provided"));

  // An errored assistant is omitted exactly like an aborted one.
  Message errorPair[2];
  errorPair[0] = Message_AssistantEmpty(arena);
  errorPair[0].Stop = StopReason::Error;
  errorPair[0].ErrorMessage = SVL("boom");
  errorPair[1] = UserMessage(arena, "continue");
  Array<Message> afterError = SessionReplay_TransformMessages(arena, errorPair, 2);
  CHECK_ALL(afterError.Count == 1, afterError.Data[0].Kind == Role::User);

  // A clean tool-use turn whose result is already present needs no synthesis.
  Message complete[3];
  complete[0] = Message_AssistantEmpty(arena);
  complete[0].Stop = StopReason::ToolUse;
  Array_Push(&complete[0].Content, Content_ToolCall(SVL("call-d"), SVL("read"), SVL("{}")));
  complete[1] = Message_ToolResult(arena, SVL("call-d"), SVL("read"), SVL("ok"), false);
  complete[2] = UserMessage(arena, "next");
  Array<Message> untouched = SessionReplay_TransformMessages(arena, complete, 3);
  CHECK(untouched.Count == 3);
}

static void TestMetadata(Arena* arena) {
  Array<SessionRecord> records = BuildValidTrace(arena);
  SessionMetadataBuildOptions options = {};
  SessionInfo info = {};
  CHECK(SessionResult_IsOk(
      SessionMetadata_Build(arena, records.Data, records.Count, &options, &info)));
  CHECK_ALL(info.MessageCount == 3, info.UserMessageCount == 1, info.AssistantMessageCount == 1,
            info.ToolResultMessageCount == 1, info.ToolCallCount == 1, info.CompactionCount == 0,
            info.TotalTokens == 19, info.InputTokens == 10, info.OutputTokens == 2,
            info.CacheReadTokens == 3, info.CacheWriteTokens == 4, info.ReasoningTokens == 1,
            StrView_EqCStr(info.CurrentProvider, "anthropic"),
            StrView_EqCStr(info.CurrentModel, "claude-test"), StrView_EqCStr(info.DisplayTitle, "hello"),
            info.ExplicitName.Length == 0, info.LastActivityAt == records.Data[3].CreatedAt,
            !info.Busy);

  Array<SessionRecord> withExtras = Array_Make<SessionRecord>(arena, 6);
  for (u32 i = 0; i < records.Count; i++) Array_Push(&withExtras, records.Data[i]);

  SessionRecord compaction =
      BaseRecord(records.Data[0].SessionId, 5, 90, SessionRecordKind::Compaction);
  compaction.CreatedAt = records.Data[3].CreatedAt + 50;
  compaction.CompactionEntry.ParentId = records.Data[3].EntryId;
  compaction.CompactionEntry.BaseEntryId = records.Data[0].EntryId;
  compaction.CompactionEntry.ThroughEntryId = records.Data[3].EntryId;
  compaction.CompactionEntry.SourceHeadId = records.Data[3].EntryId;
  compaction.CompactionEntry.Summary =
      Message_User(arena, SVL("Summary of previous conversation:\nsummary"));
  compaction.CompactionEntry.HasUsage = true;
  compaction.CompactionEntry.Use = Usage{20, 5, 2, 1, 7, 40};
  Array_Push(&withExtras, compaction);

  SessionRecord name = BaseRecord(records.Data[0].SessionId, 6, 91, SessionRecordKind::SessionName);
  name.CreatedAt = compaction.CreatedAt + 50;
  name.SessionNameEntry.Name = SVL("  Named\n\tSession  ");
  Array_Push(&withExtras, name);

  SessionMetadataBuildOptions busyOptions = {};
  busyOptions.Busy = true;
  busyOptions.NowMs = name.CreatedAt + 1000;
  SessionInfo busyInfo = {};
  CHECK(SessionResult_IsOk(
      SessionMetadata_Build(arena, withExtras.Data, withExtras.Count, &busyOptions, &busyInfo)));
  CHECK_ALL(busyInfo.CompactionCount == 1, busyInfo.TotalTokens == 19 + 40,
            StrView_EqCStr(busyInfo.ExplicitName, "  Named\n\tSession  "),
            StrView_EqCStr(busyInfo.DisplayTitle, "Named Session"), busyInfo.Busy,
            busyInfo.WallSpanMs == busyOptions.NowMs - records.Data[0].CreatedAt,
            busyInfo.LastActivityAt == compaction.CreatedAt);

  SessionRecord clear = BaseRecord(records.Data[0].SessionId, 7, 92, SessionRecordKind::SessionName);
  clear.CreatedAt = name.CreatedAt + 1;
  clear.SessionNameEntry.Name = {};
  Array_Push(&withExtras, clear);
  SessionInfo clearedInfo = {};
  CHECK(SessionResult_IsOk(
      SessionMetadata_Build(arena, withExtras.Data, withExtras.Count, &options, &clearedInfo)));
  CHECK_ALL(clearedInfo.ExplicitName.Length == 0, StrView_EqCStr(clearedInfo.DisplayTitle, "hello"));

  StrView flattened = SessionMetadata_FlattenDisplayText(
      arena, SVL(" \talpha\n\x01"
                 "beta \xF0\x9F\x98\x80 tail"),
      15);
  CHECK_ALL(StrView_EqCStr(flattened, "alpha beta \xF0\x9F\x98\x80"), flattened.Length == 15);
}

static SessionResult RepositoryCreate(SessionRepository* repository, Arena* arena, DurableId id,
                                      StrView projectKey, SessionLog* outLog, SessionInfo* outInfo) {
  SessionCreateOptions options = {};
  options.RequestedId = id;
  options.ProjectKey = projectKey;
  options.WorkingDirectory = SVL("C:\\work\\clank");
  options.Label = SVL("contract");
  return repository->Create(repository->Ctx, arena, &options, outLog, outInfo);
}

// Shared v2 contract exercised against both MemoryRepository and
// JsonlRepository: create/open exclusivity, append + duplicate-parent
// rejection, naming, fork, and project-scoped listing.
static void RunRepositoryContract(Arena* arena, SessionRepository* repository, StrView projectKey,
                                  u8 seed) {
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
  CHECK(repository->Open(repository->Ctx, arena, &open, &secondWriter, &secondInfo).Status ==
        SessionStatus::Busy);

  Array<SessionRecord> initial = {};
  CHECK(SessionResult_IsOk(SessionLog_ReadAll(&writer, arena, &initial)));
  CHECK_ALL(initial.Count == 1, initial.Data[0].Kind == SessionRecordKind::SessionStarted);

  SessionRecord user = {};
  user.EntryId = TestId((u8)(seed + 1));
  user.Kind = SessionRecordKind::Message;
  user.MessageEntry.ParentId = initial.Data[0].EntryId;
  user.MessageEntry.Value = UserMessage(arena, "persist me");
  CHECK(SessionResult_IsOk(SessionLog_Append(&writer, &user)));
  CHECK_ALL(user.Seq == 2, DurableId_Eq(user.SessionId, sessionId));

  SessionRecord invalid = {};
  invalid.EntryId = TestId((u8)(seed + 2));
  invalid.Kind = SessionRecordKind::Message;
  invalid.MessageEntry.ParentId = TestId(250);
  invalid.MessageEntry.Value = UserMessage(arena, "bad parent");
  CHECK(SessionLog_Append(&writer, &invalid).Status == SessionStatus::InvalidArgument);

  SessionRecord malformedTool = {};
  malformedTool.EntryId = TestId((u8)(seed + 5));
  malformedTool.Kind = SessionRecordKind::Message;
  malformedTool.MessageEntry.ParentId = user.EntryId;
  malformedTool.MessageEntry.Value = AssistantToolMessage(arena);
  malformedTool.MessageEntry.Value.Content.Data[1].ToolArgsJson = SVL("not-json");
  CHECK(SessionResult_IsOk(SessionLog_Append(&writer, &malformedTool)));

  Array<SessionRecord> afterInvalid = {};
  CHECK(SessionResult_IsOk(SessionLog_ReadAll(&writer, arena, &afterInvalid)));
  CHECK(afterInvalid.Count == 3);

  SessionRecord name = {};
  name.EntryId = TestId((u8)(seed + 3));
  name.Kind = SessionRecordKind::SessionName;
  name.SessionNameEntry.Name = SVL("renamed");
  CHECK(SessionResult_IsOk(SessionLog_Append(&writer, &name)));
  SessionLog_Close(&writer);

  SessionInfo inspected = {};
  CHECK(SessionResult_IsOk(repository->Resolve(repository->Ctx, arena, sessionId, &inspected)));
  CHECK_ALL(StrView_EqCStr(inspected.ExplicitName, "renamed"),
            StrView_EqCStr(inspected.DisplayTitle, "renamed"),
            StrView_EqCStr(inspected.Label, "contract"));

  open.Writable = false;
  SessionLog reader = {};
  CHECK(SessionResult_IsOk(repository->Open(repository->Ctx, arena, &open, &reader, &info)));
  Array<SessionRecord> recovered = {};
  CHECK(SessionResult_IsOk(SessionLog_ReadAll(&reader, arena, &recovered)));
  CHECK_ALL(recovered.Count == 4,
            StrView_EqCStr(
                recovered.Data[2].MessageEntry.Value.Content.Data[1].ToolArgsJson,
                "not-json"));
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
  CHECK(SessionResult_IsOk(repository->Fork(repository->Ctx, arena, &fork, &forkLog, &forkInfo)));
  Array<SessionRecord> forkRecords = {};
  CHECK(SessionResult_IsOk(SessionLog_ReadAll(&forkLog, arena, &forkRecords)));
  CHECK_ALL(forkRecords.Count == 2, forkRecords.Data[0].Kind == SessionRecordKind::SessionStarted,
            forkRecords.Data[1].Kind == SessionRecordKind::Message,
            !DurableId_Eq(forkRecords.Data[1].EntryId, forkHead),
            DurableId_Eq(forkRecords.Data[1].MessageEntry.ParentId, forkRecords.Data[0].EntryId),
            DurableId_Eq(forkRecords.Data[0].SessionStarted.ForkedFromSessionId, sessionId),
            DurableId_Eq(forkRecords.Data[0].SessionStarted.ForkedFromEntryId, forkHead),
            DurableId_Eq(forkInfo.ForkedFromSessionId, sessionId),
            DurableId_Eq(forkInfo.ForkedFromEntryId, forkHead),
            StrView_EqCStr(forkInfo.DisplayTitle, "persist me"));
  SessionLog_Close(&forkLog);

  Array<SessionInfo> listed = {};
  SessionListOptions list = {};
  list.Scope = SessionListScope::CurrentProject;
  list.ProjectKey = open.ProjectKey;
  CHECK(SessionResult_IsOk(repository->List(repository->Ctx, arena, &list, &listed)));
  CHECK(listed.Count == 2);
  SessionInfo resolved = {};
  CHECK(SessionResult_IsOk(repository->Resolve(repository->Ctx, arena, sessionId, &resolved)));
  CHECK(StrView_Eq(resolved.ProjectKey, open.ProjectKey));
}

static void TestMemoryRepository(Arena* arena) {
  MemoryRepository memory = {};
  MemoryRepository_Init(&memory);
  SessionRepository repository = MemoryRepository_AsRepository(&memory);
  RunRepositoryContract(arena, &repository, SVL("clank-memory-contract"), 70);
  repository.Close(repository.Ctx);

  // Deterministic counter: ReadAll only increments for an actual
  // SessionLog_ReadAll call; ordinary appends never trigger one, matching
  // "repository ReadAll remains initialization/browsing only".
  MemoryRepository_Init(&memory);
  repository = MemoryRepository_AsRepository(&memory);
  DurableId countedId = TestId(85);
  StrView countedProject = SVL("clank-memory-counters");
  SessionLog counted = {};
  SessionInfo countedInfo = {};
  CHECK(SessionResult_IsOk(
      RepositoryCreate(&repository, arena, countedId, countedProject, &counted, &countedInfo)));
  u64 baseline = memory.ReadAllCalls;
  Array<SessionRecord> initialCounted = {};
  CHECK(SessionResult_IsOk(SessionLog_ReadAll(&counted, arena, &initialCounted)));
  CHECK(memory.ReadAllCalls == baseline + 1);
  DurableId head = initialCounted.Data[0].EntryId;
  for (u32 i = 0; i < 5; i++) {
    SessionRecord message = {};
    message.EntryId = TestId((u8)(150 + i));
    message.Kind = SessionRecordKind::Message;
    message.MessageEntry.ParentId = head;
    message.MessageEntry.Value = UserMessage(arena, "chained");
    CHECK(SessionResult_IsOk(SessionLog_Append(&counted, &message)));
    head = message.EntryId;
  }
  CHECK(memory.ReadAllCalls == baseline + 1);
  SessionLog_Close(&counted);
  repository.Close(repository.Ctx);

  MemoryRepository_Init(&memory);
  repository = MemoryRepository_AsRepository(&memory);
  DurableId sessionId = TestId(90);
  StrView projectKey = SVL("clank-memory-failure");
  SessionLog writer = {};
  SessionInfo info = {};
  CHECK(SessionResult_IsOk(RepositoryCreate(&repository, arena, sessionId, projectKey, &writer, &info)));
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
  CHECK(SessionResult_IsOk(repository.Open(repository.Ctx, arena, &open, &reader, &info)));
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
  CHECK(SessionResult_IsOk(
      RepositoryCreate(&repository, arena, firstId, SVL("memory-project-a"), &firstLog, &firstInfo)));
  CHECK(SessionResult_IsOk(RepositoryCreate(&repository, arena, secondId, SVL("memory-project-b"),
                                            &secondLog, &secondInfo)));
  Array<SessionRecord> firstRoot = {};
  Array<SessionRecord> secondRoot = {};
  CHECK(SessionResult_IsOk(SessionLog_ReadAll(&firstLog, arena, &firstRoot)));
  CHECK(SessionResult_IsOk(SessionLog_ReadAll(&secondLog, arena, &secondRoot)));
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
  CHECK(SessionResult_IsOk(SessionLog_Append(&firstLog, &firstMessage)));
  CHECK(SessionResult_IsOk(SessionLog_Append(&secondLog, &secondMessage)));
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
  CHECK(SessionResult_IsOk(repository.List(repository.Ctx, arena, &currentList, &currentItems)));
  CHECK_ALL(currentItems.Count == 1, StrView_EqCStr(currentItems.Data[0].DisplayTitle, "named first"));
  SessionListOptions allList = {};
  allList.Scope = SessionListScope::AllProjects;
  Array<SessionInfo> allItems = {};
  CHECK(SessionResult_IsOk(repository.List(repository.Ctx, arena, &allList, &allItems)));
  CHECK_ALL(allItems.Count == 2, DurableId_Eq(allItems.Data[0].SessionId, secondId),
            allItems.Data[0].LastActivityAt > allItems.Data[1].LastActivityAt);
  SessionInfo globallyResolved = {};
  CHECK(SessionResult_IsOk(repository.Resolve(repository.Ctx, arena, secondId, &globallyResolved)));
  CHECK(StrView_EqCStr(globallyResolved.ProjectKey, "memory-project-b"));
  SessionOpenOptions clearOpen = {};
  clearOpen.SessionId = firstId;
  clearOpen.ProjectKey = SVL("memory-project-a");
  clearOpen.Writable = true;
  SessionLog clearLog = {};
  CHECK(SessionResult_IsOk(
      repository.Open(repository.Ctx, arena, &clearOpen, &clearLog, &globallyResolved)));
  SessionRecord clearName = {};
  clearName.EntryId = TestId(97);
  clearName.Kind = SessionRecordKind::SessionName;
  CHECK(SessionResult_IsOk(SessionLog_Append(&clearLog, &clearName)));
  SessionLog_Close(&clearLog);
  CHECK(SessionResult_IsOk(repository.Resolve(repository.Ctx, arena, firstId, &globallyResolved)));
  CHECK_ALL(globallyResolved.ExplicitName.Length == 0,
            StrView_EqCStr(globallyResolved.DisplayTitle, "first project"));

  repository.Close(repository.Ctx);
}

static StrView JsonlArtifactPath(Arena* arena, StrView root, StrView project, DurableId sessionId,
                                 const char* suffix) {
  StrView sessions = Fs_Join(arena, root, SVL("sessions"));
  StrView version = Fs_Join(arena, sessions, SVL("v2"));
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

static void RemoveDir(Arena* arena, StrView path) {
  StrBuf native = StrBuf_Make(arena, path.Length + 1);
  StrBuf_Append(&native, path);
  SESSION_RMDIR(StrBuf_CStr(&native));
}

static void RemoveJsonlArtifacts(Arena* arena, StrView root, StrView project, DurableId sessionId) {
  RemovePath(arena, JsonlArtifactPath(arena, root, project, sessionId, ".jsonl"));
  RemovePath(arena, JsonlArtifactPath(arena, root, project, sessionId, ".lock"));
}

static void RemoveJsonlDirectories(Arena* arena, StrView root, StrView project) {
  StrView sessions = Fs_Join(arena, root, SVL("sessions"));
  StrView version = Fs_Join(arena, sessions, SVL("v2"));
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
  CHECK(SessionResult_IsOk(
      RepositoryCreate(&repository, arena, scopeIdA, scopeProjectA, &scopeLogA, &scopeInfoA)));
  SessionLog duplicateScopeLog = {};
  SessionInfo duplicateScopeInfo = {};
  CHECK(RepositoryCreate(&repository, arena, scopeIdA, scopeProjectB, &duplicateScopeLog,
                        &duplicateScopeInfo)
            .Status == SessionStatus::Conflict);
  CHECK(SessionResult_IsOk(
      RepositoryCreate(&repository, arena, scopeIdB, scopeProjectB, &scopeLogB, &scopeInfoB)));
  SessionLog_Close(&scopeLogA);
  SessionLog_Close(&scopeLogB);
  SessionListOptions allProjects = {};
  allProjects.Scope = SessionListScope::AllProjects;
  Array<SessionInfo> allProjectItems = {};
  CHECK(SessionResult_IsOk(repository.List(repository.Ctx, arena, &allProjects, &allProjectItems)));
  CHECK(allProjectItems.Count == 2);
  SessionInfo scopeResolved = {};
  CHECK(SessionResult_IsOk(repository.Resolve(repository.Ctx, arena, scopeIdB, &scopeResolved)));
  CHECK(StrView_Eq(scopeResolved.ProjectKey, scopeProjectB));

  // Forking through the harness derives the source session/head/project from
  // the harness's own maintained state -- no ReadAll is issued to build it.
  SessionLog harnessLog = {};
  SessionInfo harnessInfo = {};
  CHECK(SessionResult_IsOk(
      RepositoryCreate(&repository, arena, harnessId, harnessProject, &harnessLog, &harnessInfo)));
  Array<SessionRecord> harnessRoot = {};
  CHECK(SessionResult_IsOk(SessionLog_ReadAll(&harnessLog, arena, &harnessRoot)));
  SessionRecord harnessMessage = {};
  harnessMessage.EntryId = TestId(161);
  harnessMessage.Kind = SessionRecordKind::Message;
  harnessMessage.MessageEntry.ParentId = harnessRoot.Data[0].EntryId;
  harnessMessage.MessageEntry.Value = UserMessage(arena, "fork through harness");
  CHECK(SessionResult_IsOk(SessionLog_Append(&harnessLog, &harnessMessage)));
  AgentHarnessConfig harnessConfig = {};
  AgentHarness harness = {};
  CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, harnessLog, &harnessConfig)));
  SessionLog harnessFork = {};
  SessionInfo harnessForkInfo = {};
  CHECK(SessionResult_IsOk(AgentHarness_Fork(arena, &harness, &repository, harnessMessage.EntryId,
                                            harnessForkId, SVL("harness fork"), &harnessFork,
                                            &harnessForkInfo)));
  Array<SessionRecord> harnessForkRecords = {};
  CHECK(SessionResult_IsOk(SessionLog_ReadAll(&harnessFork, arena, &harnessForkRecords)));
  CHECK_ALL(harnessForkRecords.Count == 2,
            harnessForkRecords.Data[1].Kind == SessionRecordKind::Message,
            DurableId_Eq(harnessForkRecords.Data[0].SessionStarted.ForkedFromSessionId, harnessId),
            DurableId_Eq(harnessForkRecords.Data[0].SessionStarted.ForkedFromEntryId,
                         harnessMessage.EntryId));
  SessionLog_Close(&harnessFork);
  CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));

  SessionLog writer = {};
  SessionInfo info = {};
  CHECK(SessionResult_IsOk(
      RepositoryCreate(&repository, arena, repairId, repairProject, &writer, &info)));
  CHECK(ChildSeesLockBusy(arena,
                         JsonlArtifactPath(arena, root, repairProject, repairId, ".lock")));
  Array<SessionRecord> rootRecords = {};
  CHECK(SessionResult_IsOk(SessionLog_ReadAll(&writer, arena, &rootRecords)));
  SessionRecord user = {};
  user.EntryId = TestId(141);
  user.Kind = SessionRecordKind::Message;
  user.MessageEntry.ParentId = rootRecords.Data[0].EntryId;
  user.MessageEntry.Value = UserMessage(arena, "before crash");
  CHECK(SessionResult_IsOk(SessionLog_Append(&writer, &user)));
  SessionLog_Close(&writer);

  StrView repairPath = JsonlArtifactPath(arena, root, repairProject, repairId, ".jsonl");
  AppendRaw(arena, repairPath, "{\"torn\":");
  SessionOpenOptions open = {};
  open.SessionId = repairId;
  open.ProjectKey = repairProject;
  open.Writable = true;
  CHECK(SessionResult_IsOk(repository.Open(repository.Ctx, arena, &open, &writer, &info)));
  Array<SessionRecord> repaired = {};
  CHECK(SessionResult_IsOk(SessionLog_ReadAll(&writer, arena, &repaired)));
  CHECK(repaired.Count == 2);
  SessionLog_Close(&writer);

  StrView repairedBytes = {};
  CHECK(ConfigFile_ReadAll(arena, repairPath, &repairedBytes) == ConfigFileReadResult::Success);
  CHECK_ALL(repairedBytes.Length > 0, repairedBytes.Data[repairedBytes.Length - 1] == '\n',
            !StrView_Contains(repairedBytes, "\"torn\""));

  CHECK(SessionResult_IsOk(
      RepositoryCreate(&repository, arena, corruptId, corruptProject, &writer, &info)));
  SessionLog_Close(&writer);
  StrView corruptPath = JsonlArtifactPath(arena, root, corruptProject, corruptId, ".jsonl");
  AppendRaw(arena, corruptPath, "not-json\n");
  SessionListOptions corruptList = {};
  corruptList.Scope = SessionListScope::CurrentProject;
  corruptList.ProjectKey = corruptProject;
  u32 skippedCount = 0;
  corruptList.SkippedCount = &skippedCount;
  Array<SessionInfo> corruptItems = {};
  CHECK(SessionResult_IsOk(repository.List(repository.Ctx, arena, &corruptList, &corruptItems)));
  CHECK_ALL(corruptItems.Count == 0, skippedCount == 1);
  SessionInfo corruptInfo = {};
  CHECK(repository.Resolve(repository.Ctx, arena, corruptId, &corruptInfo).Status ==
        SessionStatus::Corrupt);
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
  StrView version = Fs_Join(arena, sessions, SVL("v2"));
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

// v2 is an intentionally incompatible schema stored under sessions/v2; v1
// data (sessions/v1) is never migrated, read, listed, or opened. Repository
// discovery must resolve exclusively to v2, and JsonlRepository surfaces a
// one-time HasUnmigratedV1 signal a caller can use to tell the user their old
// sessions were left behind rather than silently vanishing.
static void TestJsonlV1NotMigrated(Arena* arena) {
  StrView root = SVL("clank_jsonl_v1_test");
  StrView project = SVL("clank-jsonl-v1-project");
  DurableId legacyId = TestId(200);
  DurableId v2Id = TestId(210);

  StrView sessionsDir = Fs_Join(arena, root, SVL("sessions"));
  StrView v1Dir = Fs_Join(arena, sessionsDir, SVL("v1"));
  StrView v2Dir = Fs_Join(arena, sessionsDir, SVL("v2"));
  StrView v1ProjectDir = Fs_Join(arena, v1Dir, project);

  StrBuf legacyFileName = StrBuf_Make(arena, DURABLE_ID_CHAR_COUNT + 8);
  StrBuf_Append(&legacyFileName, DurableId_View(&legacyId));
  StrBuf_AppendCStr(&legacyFileName, ".jsonl");
  StrView legacyFilePath = Fs_Join(arena, v1ProjectDir, StrBuf_View(&legacyFileName));

  // Simulate a v1 install left behind by an older Clank build: a project
  // directory containing one plausibly-named session file. Its content never
  // has to be valid v2 JSON, since v2 must never read it at all.
  CHECK(Fs_MkdirP(arena, v1ProjectDir));
  {
    StrBuf native = StrBuf_Make(arena, legacyFilePath.Length + 1);
    StrBuf_Append(&native, legacyFilePath);
    FILE* file = fopen(StrBuf_CStr(&native), "wb");
    CHECK(file != nullptr);
    if (file) {
      fwrite("not-a-v2-record\n", 1, 16, file);
      fclose(file);
    }
  }

  JsonlRepository jsonl = {};
  JsonlRepository_Init(&jsonl, root);
  CHECK(jsonl.HasUnmigratedV1);
  SessionRepository repository = JsonlRepository_AsRepository(&jsonl);

  // The v1 session is never listed or resolvable through the v2 repository.
  SessionListOptions allProjects = {};
  allProjects.Scope = SessionListScope::AllProjects;
  Array<SessionInfo> items = {};
  CHECK(SessionResult_IsOk(repository.List(repository.Ctx, arena, &allProjects, &items)));
  CHECK(items.Count == 0);
  SessionInfo resolved = {};
  CHECK(repository.Resolve(repository.Ctx, arena, legacyId, &resolved).Status ==
        SessionStatus::NotFound);
  SessionOpenOptions openLegacy = {};
  openLegacy.SessionId = legacyId;
  openLegacy.ProjectKey = project;
  SessionLog legacyLog = {};
  SessionInfo legacyInfo = {};
  CHECK(repository.Open(repository.Ctx, arena, &openLegacy, &legacyLog, &legacyInfo).Status ==
        SessionStatus::NotFound);

  // Once a v2 session exists under this data root, the "not migrated" signal
  // clears -- v2 now has its own, independent data here.
  SessionResult createdV2 = RepositoryCreate(&repository, arena, v2Id, project, &legacyLog, &legacyInfo);
  CHECK(SessionResult_IsOk(createdV2));
  SessionLog_Close(&legacyLog);
  repository.Close(repository.Ctx);

  JsonlRepository reopened = {};
  JsonlRepository_Init(&reopened, root);
  CHECK(!reopened.HasUnmigratedV1);
  SessionRepository secondRepository = JsonlRepository_AsRepository(&reopened);
  secondRepository.Close(secondRepository.Ctx);

  RemovePath(arena, legacyFilePath);
  RemoveDir(arena, v1ProjectDir);
  RemoveDir(arena, v1Dir);
  RemoveJsonlArtifacts(arena, root, project, v2Id);
  RemoveJsonlDirectories(arena, root, project);
  RemoveDir(arena, v2Dir);
  RemoveDir(arena, sessionsDir);
  RemoveDir(arena, root);
}

void TestSession() {
  Arena arena = ArenaCreate();
  TestDurableIds();
  TestRecordCodec(&arena);
  TestRecordClone();
  TestRecordValidation(&arena);
  TestReducer(&arena);
  TestReplay(&arena);
  TestMetadata(&arena);
  TestMemoryRepository(&arena);
  TestJsonlRepository(&arena);
  TestJsonlV1NotMigrated(&arena);
  ArenaRelease(&arena);
}
