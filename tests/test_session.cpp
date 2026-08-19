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
  message.Use.Reasoning = 1;
  message.Model = SVL("claude-test");
  message.Provider = SVL("anthropic");
  Array_Push(&message.Content, Content_Text(SVL("checking")));
  Array_Push(&message.Content,
             Content_ToolCall(SVL("call-1"), SVL("read"), SVL("{\"path\":\"a.txt\"}")));
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
            StrView_EqCStr(decoded.MessageEntry.Value.Model, "claude-test"));

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

  // A record written before the vendor-native protocols were removed carries
  // `signature`, `redacted`, and `cacheWrite`. Those fields are gone, but the
  // extra keys must not stop an existing session log from loading.
  const char* preMigration =
      "{\"schemaVersion\":2,\"sessionId\":\"AQIDBAUGBwgJCgsMDQ4PEA\",\"seq\":2,"
      "\"entryId\":\"FBUWFxgZGhscHR4fICEiIw\",\"type\":\"message\","
      "\"createdAt\":1700000000002,\"payload\":{\"parentId\":"
      "\"AQIDBAUGBwgJCgsMDQ4PEA\",\"message\":{\"role\":\"assistant\","
      "\"content\":[{\"kind\":\"text\",\"text\":\"checking\",\"signature\":\"\"},"
      "{\"kind\":\"thinking\",\"text\":\"pondering\",\"signature\":\"sig-1\","
      "\"redacted\":false},{\"kind\":\"tool_call\",\"toolId\":\"call-1\","
      "\"toolName\":\"read\",\"argumentsJson\":\"{}\",\"signature\":\"fc-1\"}],"
      "\"timestamp\":1700000000001,\"stopReason\":\"tool_use\",\"usage\":{"
      "\"input\":10,\"output\":2,\"cacheRead\":3,\"cacheWrite\":4,"
      "\"reasoning\":1,\"totalTokens\":0},\"errorMessage\":\"\","
      "\"model\":\"claude-test\",\"provider\":\"anthropic\"}}}";
  CHECK(SessionResult_IsOk(SessionRecord_Decode(arena, SV(preMigration), &decoded)));
  CHECK_ALL(decoded.MessageEntry.Value.Content.Count == 3,
            StrView_EqCStr(decoded.MessageEntry.Value.Content.Data[1].Text, "pondering"),
            decoded.MessageEntry.Value.Use.CacheRead == 3,
            decoded.MessageEntry.Value.Use.Input == 10);

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
  compaction.CompactionEntry.Use = Usage{11, 12, 13, 15, 65};
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
  // A compaction written before file tracking existed carries neither key and
  // must still decode, as empty lists.
  CHECK_ALL(!StrView_Contains(StrBuf_View(&noUsageJson), "\"readFiles\""),
            !StrView_Contains(StrBuf_View(&noUsageJson), "\"modifiedFiles\""),
            decoded.CompactionEntry.ReadFiles.Count == 0,
            decoded.CompactionEntry.ModifiedFiles.Count == 0);

  StrView readFiles[] = {SVL("docs/a.md")};
  StrView modifiedFiles[] = {SVL("src/b.cpp"), SVL("src/c.cpp")};
  compaction.CompactionEntry.ReadFiles = Array<StrView>{readFiles, 1, 1, nullptr};
  compaction.CompactionEntry.ModifiedFiles = Array<StrView>{modifiedFiles, 2, 2, nullptr};
  StrBuf filesJson = StrBuf_Make(arena, 512);
  CHECK(SessionResult_IsOk(SessionRecord_Encode(&filesJson, &compaction)));
  CHECK(SessionResult_IsOk(SessionRecord_Decode(arena, StrBuf_View(&filesJson), &decoded)));
  CHECK_ALL(decoded.CompactionEntry.ReadFiles.Count == 1,
            StrView_EqCStr(decoded.CompactionEntry.ReadFiles.Data[0], "docs/a.md"),
            decoded.CompactionEntry.ModifiedFiles.Count == 2,
            StrView_EqCStr(decoded.CompactionEntry.ModifiedFiles.Data[0], "src/b.cpp"),
            StrView_EqCStr(decoded.CompactionEntry.ModifiedFiles.Data[1], "src/c.cpp"));
  compaction.CompactionEntry.ReadFiles = {};
  compaction.CompactionEntry.ModifiedFiles = {};

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
  started.SessionStarted.WorkingDirectory = SVL("C:\\work\\clanker");
  started.SessionStarted.ProjectKey = SVL("clanker-1234");
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
            info.TotalTokens == 15, info.InputTokens == 10, info.OutputTokens == 2,
            info.CacheReadTokens == 3, info.ReasoningTokens == 1,
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
  compaction.CompactionEntry.Use = Usage{20, 5, 2, 7, 40};
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
  CHECK_ALL(busyInfo.CompactionCount == 1, busyInfo.TotalTokens == 15 + 40,
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
  options.WorkingDirectory = SVL("C:\\work\\clanker");
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
  fork.WorkingDirectory = SVL("C:\\work\\clanker");
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
  RunRepositoryContract(arena, &repository, SVL("clanker-memory-contract"), 70);
  repository.Close(repository.Ctx);

  // Deterministic counter: ReadAll only increments for an actual
  // SessionLog_ReadAll call; ordinary appends never trigger one, matching
  // "repository ReadAll remains initialization/browsing only".
  MemoryRepository_Init(&memory);
  repository = MemoryRepository_AsRepository(&memory);
  DurableId countedId = TestId(85);
  StrView countedProject = SVL("clanker-memory-counters");
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
  StrView projectKey = SVL("clanker-memory-failure");
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

// Gives a freshly created session the user message that makes it durable. A
// session with no user content is deliberately never written to disk, so any
// fixture that must survive a close -- or be visible to listing, resolution,
// or another process -- has to say something first.
static void PersistWithUserMessage(Arena* arena, SessionLog* log,
                                   DurableId entryId) {
  Array<SessionRecord> records = {};
  CHECK(SessionResult_IsOk(SessionLog_ReadAll(log, arena, &records)));
  SessionRecord message = {};
  message.EntryId = entryId;
  message.Kind = SessionRecordKind::Message;
  message.MessageEntry.ParentId = records.Data[0].EntryId;
  message.MessageEntry.Value = UserMessage(arena, "persisted");
  CHECK(SessionResult_IsOk(SessionLog_Append(log, &message)));
}

static void TestJsonlRepository(Arena* arena) {
  StrView root = SVL("clanker_jsonl_test");
  StrView contractProject = SVL("clanker-jsonl-contract");
  StrView repairProject = SVL("clanker-jsonl-repair");
  StrView corruptProject = SVL("clanker-jsonl-corrupt");
  StrView harnessProject = SVL("clanker-jsonl-harness");
  StrView scopeProjectA = SVL("clanker-jsonl-scope-a");
  StrView scopeProjectB = SVL("clanker-jsonl-scope-b");
  StrView deferProject = SVL("clanker-jsonl-defer");
  DurableId contractId = TestId(120);
  DurableId forkId = TestId(130);
  DurableId repairId = TestId(140);
  DurableId corruptId = TestId(150);
  DurableId harnessId = TestId(160);
  DurableId harnessForkId = TestId(170);
  DurableId scopeIdA = TestId(180);
  DurableId scopeIdB = TestId(190);
  DurableId abandonedId = TestId(200);
  DurableId deferredId = TestId(210);
  DurableId forkSourceId = TestId(220);
  DurableId forkTargetId = TestId(230);
  RemoveJsonlArtifacts(arena, root, contractProject, contractId);
  RemoveJsonlArtifacts(arena, root, contractProject, forkId);
  RemoveJsonlArtifacts(arena, root, repairProject, repairId);
  RemoveJsonlArtifacts(arena, root, corruptProject, corruptId);
  RemoveJsonlArtifacts(arena, root, harnessProject, harnessId);
  RemoveJsonlArtifacts(arena, root, harnessProject, harnessForkId);
  RemoveJsonlArtifacts(arena, root, scopeProjectA, scopeIdA);
  RemoveJsonlArtifacts(arena, root, scopeProjectB, scopeIdB);
  RemoveJsonlArtifacts(arena, root, deferProject, abandonedId);
  RemoveJsonlArtifacts(arena, root, deferProject, deferredId);
  RemoveJsonlArtifacts(arena, root, deferProject, forkSourceId);
  RemoveJsonlArtifacts(arena, root, deferProject, forkTargetId);

  JsonlRepository jsonl = {};
  JsonlRepository_Init(&jsonl, root);
  SessionRepository repository = JsonlRepository_AsRepository(&jsonl);

  // The shell tool's overflow directory is the session log path without its
  // extension, so per-session artifacts sit beside the transcript.
  StrView artifactDir =
      JsonlRepository_SessionDir(arena, &jsonl, contractProject, contractId);
  StrView logPath =
      JsonlRepository_SessionPath(arena, &jsonl, contractProject, contractId);
  StrBuf expectedLog = StrBuf_Make(arena, artifactDir.Length + 8);
  StrBuf_Append(&expectedLog, artifactDir);
  StrBuf_AppendCStr(&expectedLog, ".jsonl");
  CHECK_ALL(artifactDir.Length > 0,
            Fs_PathEquals(StrBuf_View(&expectedLog), logPath),
            StrView_Contains(artifactDir, "clanker-jsonl-contract"));
  // An unusable repository or key yields nothing rather than a bare root path.
  JsonlRepository uninitialized = {};
  CHECK_ALL(JsonlRepository_SessionDir(arena, &uninitialized, contractProject,
                                       contractId)
                    .Length == 0,
            JsonlRepository_SessionDir(arena, &jsonl, contractProject,
                                       DurableId{})
                    .Length == 0);

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
  PersistWithUserMessage(arena, &scopeLogA, TestId(181));
  PersistWithUserMessage(arena, &scopeLogB, TestId(191));
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

  // A session the user never speaks to must not leave a log behind: that is
  // what put one orphan file per launch beside every project's transcripts.
  SessionLog abandoned = {};
  SessionInfo abandonedInfo = {};
  CHECK(SessionResult_IsOk(RepositoryCreate(&repository, arena, abandonedId,
                                            deferProject, &abandoned,
                                            &abandonedInfo)));
  StrView abandonedPath =
      JsonlArtifactPath(arena, root, deferProject, abandonedId, ".jsonl");
  StrView abandonedLock =
      JsonlArtifactPath(arena, root, deferProject, abandonedId, ".lock");
  CHECK(!Fs_Exists(abandonedPath));
  Array<SessionRecord> abandonedRoot = {};
  CHECK(SessionResult_IsOk(SessionLog_ReadAll(&abandoned, arena, &abandonedRoot)));
  // The harness records the selected model at startup. That is bookkeeping
  // rather than user intent, so it must not author the file on its own.
  SessionRecord startupModel = {};
  startupModel.EntryId = TestId(201);
  startupModel.Kind = SessionRecordKind::Model;
  startupModel.ModelEntry.ParentId = abandonedRoot.Data[0].EntryId;
  startupModel.ModelEntry.Provider = SVL("anthropic");
  startupModel.ModelEntry.Model = SVL("claude-sonnet-4-6");
  CHECK(SessionResult_IsOk(SessionLog_Append(&abandoned, &startupModel)));
  CHECK(!Fs_Exists(abandonedPath));
  // Buffered does not mean unowned: the id is still claimed, so a second
  // writer is turned away as busy rather than told the session is missing.
  SessionOpenOptions abandonedOpen = {};
  abandonedOpen.SessionId = abandonedId;
  abandonedOpen.ProjectKey = deferProject;
  abandonedOpen.Writable = true;
  SessionLog abandonedSecond = {};
  SessionInfo abandonedSecondInfo = {};
  CHECK(repository
            .Open(repository.Ctx, arena, &abandonedOpen, &abandonedSecond,
                  &abandonedSecondInfo)
            .Status == SessionStatus::Busy);
  SessionLog_Close(&abandoned);
  CHECK_ALL(!Fs_Exists(abandonedPath), !Fs_Exists(abandonedLock));
  // Once released the id really is gone, so the same open now reports missing.
  CHECK(repository
            .Open(repository.Ctx, arena, &abandonedOpen, &abandonedSecond,
                  &abandonedSecondInfo)
            .Status == SessionStatus::NotFound);

  // The append that trips the trigger carries the whole buffer with it, so
  // records written before the first user message are not lost.
  SessionLog deferred = {};
  SessionInfo deferredInfo = {};
  CHECK(SessionResult_IsOk(RepositoryCreate(&repository, arena, deferredId,
                                            deferProject, &deferred,
                                            &deferredInfo)));
  StrView deferredPath =
      JsonlArtifactPath(arena, root, deferProject, deferredId, ".jsonl");
  Array<SessionRecord> deferredRoot = {};
  CHECK(SessionResult_IsOk(SessionLog_ReadAll(&deferred, arena, &deferredRoot)));
  SessionRecord deferredModel = {};
  deferredModel.EntryId = TestId(211);
  deferredModel.Kind = SessionRecordKind::Model;
  deferredModel.ModelEntry.ParentId = deferredRoot.Data[0].EntryId;
  deferredModel.ModelEntry.Provider = SVL("anthropic");
  deferredModel.ModelEntry.Model = SVL("claude-sonnet-4-6");
  CHECK(SessionResult_IsOk(SessionLog_Append(&deferred, &deferredModel)));
  CHECK(!Fs_Exists(deferredPath));
  SessionRecord deferredUser = {};
  deferredUser.EntryId = TestId(212);
  deferredUser.Kind = SessionRecordKind::Message;
  deferredUser.MessageEntry.ParentId = deferredModel.EntryId;
  deferredUser.MessageEntry.Value = UserMessage(arena, "first words");
  CHECK(SessionResult_IsOk(SessionLog_Append(&deferred, &deferredUser)));
  CHECK(Fs_Exists(deferredPath));
  SessionLog_Close(&deferred);

  SessionLog reopened = {};
  SessionInfo reopenedInfo = {};
  SessionOpenOptions deferredOpen = {};
  deferredOpen.SessionId = deferredId;
  deferredOpen.ProjectKey = deferProject;
  deferredOpen.Writable = true;
  CHECK(SessionResult_IsOk(repository.Open(repository.Ctx, arena, &deferredOpen,
                                           &reopened, &reopenedInfo)));
  Array<SessionRecord> deferredRecords = {};
  CHECK(SessionResult_IsOk(SessionLog_ReadAll(&reopened, arena, &deferredRecords)));
  CHECK_ALL(deferredRecords.Count == 3,
            deferredRecords.Data[0].Kind == SessionRecordKind::SessionStarted,
            deferredRecords.Data[1].Kind == SessionRecordKind::Model,
            deferredRecords.Data[2].Kind == SessionRecordKind::Message,
            deferredRecords.Data[2].Seq == 3);
  SessionLog_Close(&reopened);

  // Forking a source that has not yet earned a file must read it from the live
  // writer instead of reporting the session missing.
  SessionLog bufferedSource = {};
  SessionInfo bufferedSourceInfo = {};
  CHECK(SessionResult_IsOk(RepositoryCreate(&repository, arena, forkSourceId,
                                            deferProject, &bufferedSource,
                                            &bufferedSourceInfo)));
  StrView forkSourcePath =
      JsonlArtifactPath(arena, root, deferProject, forkSourceId, ".jsonl");
  Array<SessionRecord> bufferedRoot = {};
  CHECK(SessionResult_IsOk(SessionLog_ReadAll(&bufferedSource, arena, &bufferedRoot)));
  CHECK(!Fs_Exists(forkSourcePath));
  SessionForkOptions forkBuffered = {};
  forkBuffered.SourceSessionId = forkSourceId;
  forkBuffered.SourceHeadId = bufferedRoot.Data[0].EntryId;
  forkBuffered.RequestedTargetId = forkTargetId;
  forkBuffered.ProjectKey = deferProject;
  forkBuffered.WorkingDirectory = SVL("C:\\work\\clanker");
  forkBuffered.Label = SVL("buffered fork");
  SessionLog forkedLog = {};
  SessionInfo forkedInfo = {};
  CHECK(SessionResult_IsOk(repository.Fork(repository.Ctx, arena, &forkBuffered,
                                           &forkedLog, &forkedInfo)));
  Array<SessionRecord> forkedRecords = {};
  CHECK(SessionResult_IsOk(SessionLog_ReadAll(&forkedLog, arena, &forkedRecords)));
  CHECK_ALL(forkedRecords.Count == 1,
            forkedRecords.Data[0].Kind == SessionRecordKind::SessionStarted,
            DurableId_Eq(forkedRecords.Data[0].SessionStarted.ForkedFromSessionId,
                         forkSourceId));
  SessionLog_Close(&forkedLog);
  SessionLog_Close(&bufferedSource);

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
  StrView repairLockPath =
      JsonlArtifactPath(arena, root, repairProject, repairId, ".lock");
  CHECK(ChildSeesLockBusy(arena, repairLockPath));
  // The sentinel exists only while the writer holds it; releasing the lock
  // must not leave one lock file per session behind next to the log.
  CHECK(Fs_Exists(repairLockPath));
  Array<SessionRecord> rootRecords = {};
  CHECK(SessionResult_IsOk(SessionLog_ReadAll(&writer, arena, &rootRecords)));
  SessionRecord user = {};
  user.EntryId = TestId(141);
  user.Kind = SessionRecordKind::Message;
  user.MessageEntry.ParentId = rootRecords.Data[0].EntryId;
  user.MessageEntry.Value = UserMessage(arena, "before crash");
  CHECK(SessionResult_IsOk(SessionLog_Append(&writer, &user)));
  SessionLog_Close(&writer);
  CHECK(!Fs_Exists(repairLockPath));

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
  // The fixture is a valid log with a corrupt line appended, so the session
  // has to be given content before it exists on disk to be corrupted.
  PersistWithUserMessage(arena, &writer, TestId(151));
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
  RemoveJsonlArtifacts(arena, root, deferProject, abandonedId);
  RemoveJsonlArtifacts(arena, root, deferProject, deferredId);
  RemoveJsonlArtifacts(arena, root, deferProject, forkSourceId);
  RemoveJsonlArtifacts(arena, root, deferProject, forkTargetId);
  RemoveJsonlDirectories(arena, root, contractProject);
  RemoveJsonlDirectories(arena, root, repairProject);
  RemoveJsonlDirectories(arena, root, corruptProject);
  RemoveJsonlDirectories(arena, root, harnessProject);
  RemoveJsonlDirectories(arena, root, scopeProjectA);
  RemoveJsonlDirectories(arena, root, scopeProjectB);
  RemoveJsonlDirectories(arena, root, deferProject);
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

static void TestCompactionReplay(Arena* arena) {
  DurableId sessionId = TestId(1);

  // root -> user -> assistant -> toolResult -> user -> assistant, plus a
  // sibling of the first user message so an off-branch reference can be tested.
  SessionRecord root = BaseRecord(sessionId, 1, 10, SessionRecordKind::SessionStarted);
  root.SessionStarted.WorkingDirectory = SVL("C:\\work\\clanker");
  root.SessionStarted.ProjectKey = SVL("clanker-compaction");
  root.SessionStarted.Label = SVL("compaction");

  SessionRecord userOne = BaseRecord(sessionId, 2, 12, SessionRecordKind::Message);
  userOne.MessageEntry.ParentId = root.EntryId;
  userOne.MessageEntry.Value = UserMessage(arena, "one");

  SessionRecord assistantOne = BaseRecord(sessionId, 3, 14, SessionRecordKind::Message);
  assistantOne.MessageEntry.ParentId = userOne.EntryId;
  assistantOne.MessageEntry.Value = AssistantToolMessage(arena);

  SessionRecord toolResult = BaseRecord(sessionId, 4, 16, SessionRecordKind::Message);
  toolResult.MessageEntry.ParentId = assistantOne.EntryId;
  toolResult.MessageEntry.Value =
      Message_ToolResult(arena, SVL("call-1"), SVL("read"), SVL("contents"), false);

  SessionRecord userTwo = BaseRecord(sessionId, 5, 18, SessionRecordKind::Message);
  userTwo.MessageEntry.ParentId = toolResult.EntryId;
  userTwo.MessageEntry.Value = UserMessage(arena, "two");

  SessionRecord assistantTwo = BaseRecord(sessionId, 6, 20, SessionRecordKind::Message);
  assistantTwo.MessageEntry.ParentId = userTwo.EntryId;
  assistantTwo.MessageEntry.Value = UserMessage(arena, "reply");

  SessionRecord sibling = BaseRecord(sessionId, 7, 22, SessionRecordKind::Message);
  sibling.MessageEntry.ParentId = userOne.EntryId;
  sibling.MessageEntry.Value = UserMessage(arena, "sibling branch");

  auto build = [&](const SessionRecord* extra, u32 extraCount) {
    Array<SessionRecord> records = Array_Make<SessionRecord>(arena, 8 + extraCount);
    Array_Push(&records, root);
    Array_Push(&records, userOne);
    Array_Push(&records, assistantOne);
    Array_Push(&records, toolResult);
    Array_Push(&records, userTwo);
    Array_Push(&records, assistantTwo);
    Array_Push(&records, sibling);
    for (u32 i = 0; i < extraCount; i++) Array_Push(&records, extra[i]);
    return records;
  };

  // A compaction whose span stops before the trailing user turn keeps that
  // turn and everything after it verbatim.
  SessionRecord retained = BaseRecord(sessionId, 8, 24, SessionRecordKind::Compaction);
  retained.CompactionEntry.ParentId = assistantTwo.EntryId;
  retained.CompactionEntry.BaseEntryId = root.EntryId;
  retained.CompactionEntry.ThroughEntryId = toolResult.EntryId;
  retained.CompactionEntry.SourceHeadId = assistantTwo.EntryId;
  retained.CompactionEntry.Summary = UserMessage(arena, "summary one");
  {
    Array<SessionRecord> records = build(&retained, 1);
    SessionReduction reduction = {};
    CHECK(SessionResult_IsOk(
        SessionReducer_Reduce(arena, records.Data, records.Count, &reduction)));
    Array<Message> context = {};
    CHECK(SessionResult_IsOk(
        SessionReducer_Context(arena, &reduction, retained.EntryId, &context)));
    CHECK(context.Count == 3);
    if (context.Count == 3) {
      CHECK_ALL(StrView_EqCStr(context.Data[0].Content.Data[0].Text, "summary one"),
                StrView_EqCStr(context.Data[1].Content.Data[0].Text, "two"),
                StrView_EqCStr(context.Data[2].Content.Data[0].Text, "reply"));
    }
  }

  // A legacy whole-branch span (root through the head) still collapses to the
  // summary alone, so pre-retention sessions replay unchanged.
  SessionRecord legacy = BaseRecord(sessionId, 8, 26, SessionRecordKind::Compaction);
  legacy.CompactionEntry.ParentId = assistantTwo.EntryId;
  legacy.CompactionEntry.BaseEntryId = root.EntryId;
  legacy.CompactionEntry.ThroughEntryId = assistantTwo.EntryId;
  legacy.CompactionEntry.SourceHeadId = assistantTwo.EntryId;
  legacy.CompactionEntry.Summary = UserMessage(arena, "legacy summary");
  {
    Array<SessionRecord> records = build(&legacy, 1);
    SessionReduction reduction = {};
    CHECK(SessionResult_IsOk(
        SessionReducer_Reduce(arena, records.Data, records.Count, &reduction)));
    Array<Message> context = {};
    CHECK(SessionResult_IsOk(
        SessionReducer_Context(arena, &reduction, legacy.EntryId, &context)));
    CHECK(context.Count == 1);
    if (context.Count == 1)
      CHECK(StrView_EqCStr(context.Data[0].Content.Data[0].Text, "legacy summary"));
  }

  // A later compaction whose span covers an earlier summary anchor supersedes
  // it: only the newest summary and the messages after it survive.
  SessionRecord userThree = BaseRecord(sessionId, 9, 28, SessionRecordKind::Message);
  userThree.MessageEntry.ParentId = retained.EntryId;
  userThree.MessageEntry.Value = UserMessage(arena, "three");
  SessionRecord assistantThree = BaseRecord(sessionId, 10, 30, SessionRecordKind::Message);
  assistantThree.MessageEntry.ParentId = userThree.EntryId;
  assistantThree.MessageEntry.Value = UserMessage(arena, "third reply");
  SessionRecord second = BaseRecord(sessionId, 11, 32, SessionRecordKind::Compaction);
  second.CompactionEntry.ParentId = assistantThree.EntryId;
  second.CompactionEntry.BaseEntryId = root.EntryId;
  second.CompactionEntry.ThroughEntryId = userThree.EntryId;
  second.CompactionEntry.SourceHeadId = assistantThree.EntryId;
  second.CompactionEntry.Summary = UserMessage(arena, "summary two");
  {
    SessionRecord extra[4] = {retained, userThree, assistantThree, second};
    Array<SessionRecord> records = build(extra, 4);
    SessionReduction reduction = {};
    CHECK(SessionResult_IsOk(
        SessionReducer_Reduce(arena, records.Data, records.Count, &reduction)));
    Array<Message> context = {};
    CHECK(SessionResult_IsOk(
        SessionReducer_Context(arena, &reduction, second.EntryId, &context)));
    CHECK(context.Count == 2);
    if (context.Count == 2) {
      CHECK_ALL(StrView_EqCStr(context.Data[0].Content.Data[0].Text, "summary two"),
                StrView_EqCStr(context.Data[1].Content.Data[0].Text, "third reply"));
    }
  }

  // A span endpoint that exists in the session but is not an ancestor of the
  // head cannot be positioned on this branch, so replay reports corruption
  // rather than silently emitting the wrong context.
  SessionRecord offBranch = BaseRecord(sessionId, 8, 34, SessionRecordKind::Compaction);
  offBranch.CompactionEntry.ParentId = assistantTwo.EntryId;
  offBranch.CompactionEntry.BaseEntryId = root.EntryId;
  offBranch.CompactionEntry.ThroughEntryId = sibling.EntryId;
  offBranch.CompactionEntry.SourceHeadId = assistantTwo.EntryId;
  offBranch.CompactionEntry.Summary = UserMessage(arena, "off branch");
  {
    Array<SessionRecord> records = build(&offBranch, 1);
    SessionReduction reduction = {};
    CHECK(SessionResult_IsOk(
        SessionReducer_Reduce(arena, records.Data, records.Count, &reduction)));
    Array<Message> context = {};
    CHECK(SessionReducer_Context(arena, &reduction, offBranch.EntryId, &context).Status ==
          SessionStatus::Corrupt);
  }

  // An inverted span is rejected too.
  SessionRecord inverted = BaseRecord(sessionId, 8, 36, SessionRecordKind::Compaction);
  inverted.CompactionEntry.ParentId = assistantTwo.EntryId;
  inverted.CompactionEntry.BaseEntryId = toolResult.EntryId;
  inverted.CompactionEntry.ThroughEntryId = root.EntryId;
  inverted.CompactionEntry.SourceHeadId = assistantTwo.EntryId;
  inverted.CompactionEntry.Summary = UserMessage(arena, "inverted");
  {
    Array<SessionRecord> records = build(&inverted, 1);
    SessionReduction reduction = {};
    CHECK(SessionResult_IsOk(
        SessionReducer_Reduce(arena, records.Data, records.Count, &reduction)));
    Array<Message> context = {};
    CHECK(SessionReducer_Context(arena, &reduction, inverted.EntryId, &context).Status ==
          SessionStatus::Corrupt);
  }
}

void TestSession() {
  Arena arena = ArenaCreate();
  TestDurableIds();
  TestRecordCodec(&arena);
  TestRecordClone();
  TestRecordValidation(&arena);
  TestReducer(&arena);
  TestCompactionReplay(&arena);
  TestReplay(&arena);
  TestMetadata(&arena);
  TestMemoryRepository(&arena);
  TestJsonlRepository(&arena);
  ArenaRelease(&arena);
}
