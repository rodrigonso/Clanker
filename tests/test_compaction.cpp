#include "agent/compaction.h"

#include "base/arena.h"
#include "check.h"

namespace {

Message UserMessage(Arena* arena, StrView text) {
  return Message_User(arena, text);
}

Message AssistantToolCall(Arena* arena, StrView id, StrView name, StrView argsJson) {
  Message message = Message_AssistantEmpty(arena);
  Array_Push(&message.Content, Content_ToolCall(id, name, argsJson));
  message.Stop = StopReason::ToolUse;
  return message;
}

Message AssistantText(Arena* arena, StrView text, Usage use) {
  Message message = Message_AssistantEmpty(arena);
  Array_Push(&message.Content, Content_Text(text));
  message.Use = use;
  return message;
}

SessionRecord MessageRecord(DurableId parent, const Message& value) {
  SessionRecord record = {};
  record.EntryId = DurableId_Generate();
  record.Kind = SessionRecordKind::Message;
  record.MessageEntry.ParentId = parent;
  record.MessageEntry.Value = value;
  return record;
}

SessionRecord RootRecord() {
  SessionRecord record = {};
  record.EntryId = DurableId_Generate();
  record.Kind = SessionRecordKind::SessionStarted;
  return record;
}

// Branch shaped root -> user -> assistant(tool call) -> tool result -> user ->
// assistant. Each message costs exactly 4 or 5 estimated tokens, so budgets in
// the tests below land on predictable boundaries.
struct Branch {
  SessionRecord Records[6];
  const SessionRecord* Pointers[6];
  u32 Count;
};

void BuildBranch(Arena* arena, Branch* out) {
  out->Records[0] = RootRecord();
  out->Records[1] = MessageRecord(out->Records[0].EntryId, UserMessage(arena, SVL("aaaa")));
  out->Records[2] = MessageRecord(
      out->Records[1].EntryId,
      AssistantToolCall(arena, SVL("call-1"), SVL("t"), SVL("{}")));
  out->Records[3] = MessageRecord(
      out->Records[2].EntryId,
      Message_ToolResult(arena, SVL("call-1"), SVL("t"), SVL("bbbb"), false));
  out->Records[4] = MessageRecord(out->Records[3].EntryId, UserMessage(arena, SVL("cccc")));
  out->Records[5] = MessageRecord(out->Records[4].EntryId,
                                  AssistantText(arena, SVL("dddd"), Usage{}));
  out->Count = 6;
  for (u32 i = 0; i < out->Count; i++) out->Pointers[i] = &out->Records[i];
}

void TestEstimator(Arena* arena) {
  // Envelope overhead applies even to an empty message.
  Message empty = Message_AssistantEmpty(arena);
  CHECK(AgentCompaction_EstimateMessageTokens(&empty) == 4);

  // 4 content bytes at 4 bytes/token, plus overhead.
  Message user = UserMessage(arena, SVL("aaaa"));
  CHECK(AgentCompaction_EstimateMessageTokens(&user) == 5);

  // Tool name and arguments count; the internal call id does not.
  Message call = AssistantToolCall(arena, SVL("call-1"), SVL("t"), SVL("{}"));
  CHECK(AgentCompaction_EstimateMessageTokens(&call) == 4);

  // The tool result's own tool name counts alongside its content.
  Message result = Message_ToolResult(arena, SVL("call-1"), SVL("t"), SVL("bbbb"), false);
  CHECK(AgentCompaction_EstimateMessageTokens(&result) == 5);

  Message messages[2] = {user, result};
  CHECK(AgentCompaction_EstimateContextTokens(messages, 2) == 10);
  CHECK(AgentCompaction_EstimateContextTokens(nullptr, 0) == 0);
}

void TestReportedTokens(Arena* arena) {
  // No usage anywhere: reported is unavailable and the estimate is used.
  Message none[2] = {UserMessage(arena, SVL("aaaa")),
                     AssistantText(arena, SVL("dddd"), Usage{})};
  CHECK(AgentCompaction_ReportedContextTokens(none, 2) == 0);
  CHECK(AgentCompaction_ContextTokens(none, 2) == 10);

  // The newest assistant usage carries the whole prompt plus its own output.
  Usage use = {};
  use.Input = 100;
  use.CacheRead = 10;
  use.Output = 20;
  Message reported[2] = {UserMessage(arena, SVL("aaaa")),
                         AssistantText(arena, SVL("dddd"), use)};
  CHECK(AgentCompaction_ReportedContextTokens(reported, 2) == 130);
  CHECK(AgentCompaction_ContextTokens(reported, 2) == 130);

  // Messages appended after that assistant turn have not been billed, so they
  // are estimated on top.
  Message trailing[3] = {
      UserMessage(arena, SVL("aaaa")), AssistantText(arena, SVL("dddd"), use),
      Message_ToolResult(arena, SVL("call-1"), SVL("t"), SVL("bbbb"), false)};
  CHECK(AgentCompaction_ReportedContextTokens(trailing, 3) == 135);
}

void TestShouldCompact() {
  AgentCompactionPolicy policy = AgentCompactionPolicy_Default();
  CHECK_ALL(policy.Enabled, policy.ReserveTokens == 16384,
            policy.KeepRecentTokens == 20000);
  CHECK(AgentCompactionPolicy_IsValid(&policy));

  // Threshold is window minus reserve.
  CHECK(!AgentCompaction_ShouldCompact(&policy, 100000, 83616));
  CHECK(!AgentCompaction_ShouldCompact(&policy, 100000, 83615));
  CHECK(AgentCompaction_ShouldCompact(&policy, 100000, 83617));

  // A disabled or degenerate policy never fires.
  AgentCompactionPolicy disabled = policy;
  disabled.Enabled = false;
  CHECK(!AgentCompaction_ShouldCompact(&disabled, 100000, 99999));
  CHECK(!AgentCompactionPolicy_IsValid(&disabled));

  AgentCompactionPolicy zeroReserve = policy;
  zeroReserve.ReserveTokens = 0;
  CHECK(!AgentCompaction_ShouldCompact(&zeroReserve, 100000, 99999));

  AgentCompactionPolicy zeroKeep = policy;
  zeroKeep.KeepRecentTokens = 0;
  CHECK(!AgentCompaction_ShouldCompact(&zeroKeep, 100000, 99999));

  // An unknown window, or a reserve wider than the window, cannot be defended.
  CHECK(!AgentCompaction_ShouldCompact(&policy, 0, 99999));
  CHECK(!AgentCompaction_ShouldCompact(&policy, 16384, 99999));
  CHECK(!AgentCompaction_ShouldCompact(nullptr, 100000, 99999));
}

void TestCutPoint(Arena* arena) {
  Branch branch = {};
  BuildBranch(arena, &branch);
  AgentCompactionCut cut = {};

  // Budget 12 keeps the trailing user turn and its assistant reply (10 tokens)
  // but not the preceding tool result, so the fold ends at the tool result.
  CHECK(AgentCompaction_SelectCutPoint(branch.Pointers, branch.Count, 12, &cut));
  CHECK_ALL(cut.HasRetainedTail,
            DurableId_Eq(cut.BaseEntryId, branch.Records[0].EntryId),
            DurableId_Eq(cut.ThroughEntryId, branch.Records[3].EntryId));

  // The retained tail always begins at a user message, which is what keeps an
  // assistant tool call attached to its tool results.
  CHECK(branch.Records[4].MessageEntry.Value.Kind == Role::User);

  // A budget covering the whole branch leaves nothing worth folding.
  CHECK(!AgentCompaction_SelectCutPoint(branch.Pointers, branch.Count, 1000, &cut));

  // A budget too small for even the newest message folds the whole branch.
  AgentCompactionCut full = {};
  CHECK(AgentCompaction_SelectCutPoint(branch.Pointers, branch.Count, 3, &full));
  CHECK_ALL(!full.HasRetainedTail,
            DurableId_Eq(full.BaseEntryId, branch.Records[0].EntryId),
            DurableId_Eq(full.ThroughEntryId, branch.Records[5].EntryId));

  // A branch with no messages has nothing to compact.
  SessionRecord root = RootRecord();
  const SessionRecord* rootOnly[1] = {&root};
  CHECK(!AgentCompaction_SelectCutPoint(rootOnly, 1, 100, &cut));

  CHECK(!AgentCompaction_SelectCutPoint(nullptr, 0, 100, &cut));
  CHECK(!AgentCompaction_SelectCutPoint(branch.Pointers, branch.Count, 12, nullptr));
}

// A branch record carrying already-persisted compaction file lists, which is
// what a later compaction reseeds from.
SessionRecord CompactionRecord(Arena* arena, DurableId parent,
                               DurableId baseEntryId, DurableId throughEntryId,
                               const char* const* readFiles, u32 readCount,
                               const char* const* modifiedFiles, u32 modifiedCount) {
  SessionRecord record = {};
  record.EntryId = DurableId_Generate();
  record.Kind = SessionRecordKind::Compaction;
  record.CompactionEntry.ParentId = parent;
  record.CompactionEntry.BaseEntryId = baseEntryId;
  record.CompactionEntry.ThroughEntryId = throughEntryId;
  record.CompactionEntry.SourceHeadId = parent;
  record.CompactionEntry.Summary = Message_User(arena, SVL("summary"));
  record.CompactionEntry.ReadFiles = Array_Make<StrView>(arena, readCount ? readCount : 1);
  for (u32 i = 0; i < readCount; i++)
    Array_Push(&record.CompactionEntry.ReadFiles, SV(readFiles[i]));
  record.CompactionEntry.ModifiedFiles =
      Array_Make<StrView>(arena, modifiedCount ? modifiedCount : 1);
  for (u32 i = 0; i < modifiedCount; i++)
    Array_Push(&record.CompactionEntry.ModifiedFiles, SV(modifiedFiles[i]));
  return record;
}

bool PathsAre(const Array<StrView>* paths, const char* const* expected, u32 count) {
  if (paths->Count != count) return false;
  for (u32 i = 0; i < count; i++)
    if (!StrView_EqCStr(paths->Data[i], expected[i])) return false;
  return true;
}

void TestFileTracking(Arena* arena) {
  SessionRecord root = RootRecord();
  SessionRecord readB = MessageRecord(
      root.EntryId,
      AssistantToolCall(arena, SVL("c1"), SVL("read"), SVL("{\"path\":\"src/b.cpp\"}")));
  SessionRecord readA = MessageRecord(
      readB.EntryId,
      AssistantToolCall(arena, SVL("c2"), SVL("read"), SVL("{\"path\":\"src/a.cpp\"}")));
  // Reading then editing the same file must report it only as modified.
  SessionRecord editB = MessageRecord(
      readA.EntryId,
      AssistantToolCall(arena, SVL("c3"), SVL("edit"), SVL("{\"path\":\"src/b.cpp\"}")));
  SessionRecord wrote = MessageRecord(
      editB.EntryId,
      AssistantToolCall(arena, SVL("c4"), SVL("write"), SVL("{\"path\":\"src/new.cpp\"}")));
  // Neither an untracked tool nor a shell command contributes a path.
  SessionRecord shell = MessageRecord(
      wrote.EntryId,
      AssistantToolCall(arena, SVL("c5"), SVL("shell"),
                        SVL("{\"command\":\"rm src/gone.cpp\"}")));
  SessionRecord grep = MessageRecord(
      shell.EntryId,
      AssistantToolCall(arena, SVL("c6"), SVL("grep"), SVL("{\"path\":\"src/skip.cpp\"}")));
  // A tool result echoing a path is not an assistant tool call.
  SessionRecord result = MessageRecord(
      grep.EntryId,
      Message_ToolResult(arena, SVL("c6"), SVL("grep"), SVL("{\"path\":\"src/x\"}"), false));

  const SessionRecord* branch[] = {&root,  &readB,  &readA, &editB,
                                   &wrote, &shell, &grep,  &result};
  const u32 branchCount = (u32)(sizeof(branch) / sizeof(branch[0]));

  AgentFileOps ops = {};
  AgentCompaction_CollectFileOps(arena, branch, branchCount, DurableId{}, &ops);
  AgentFileLists lists = {};
  AgentCompaction_BuildFileLists(arena, &ops, &lists);

  const char* expectedRead[] = {"src/a.cpp"};
  const char* expectedModified[] = {"src/b.cpp", "src/new.cpp"};
  CHECK_ALL(PathsAre(&lists.ReadFiles, expectedRead, 1),
            PathsAre(&lists.ModifiedFiles, expectedModified, 2));

  // `through` bounds the scan: stopping at the first read excludes the edit,
  // so the file is still only-read at that point.
  AgentFileOps bounded = {};
  AgentCompaction_CollectFileOps(arena, branch, branchCount, readB.EntryId, &bounded);
  AgentFileLists boundedLists = {};
  AgentCompaction_BuildFileLists(arena, &bounded, &boundedLists);
  const char* boundedRead[] = {"src/b.cpp"};
  CHECK_ALL(PathsAre(&boundedLists.ReadFiles, boundedRead, 1),
            boundedLists.ModifiedFiles.Count == 0);

  // A prior compaction's stored lists are the only surviving record of the
  // turns it folded away, so a later compaction must reseed from them.
  const char* priorRead[] = {"docs/old.md"};
  const char* priorModified[] = {"src/older.cpp"};
  SessionRecord prior = CompactionRecord(arena, root.EntryId, root.EntryId,
                                         root.EntryId, priorRead, 1, priorModified, 1);
  SessionRecord after = MessageRecord(
      prior.EntryId,
      AssistantToolCall(arena, SVL("c7"), SVL("read"), SVL("{\"path\":\"docs/new.md\"}")));
  const SessionRecord* cumulative[] = {&root, &prior, &after};

  AgentFileOps carried = {};
  AgentCompaction_CollectFileOps(arena, cumulative, 3, DurableId{}, &carried);
  AgentFileLists carriedLists = {};
  AgentCompaction_BuildFileLists(arena, &carried, &carriedLists);
  const char* carriedRead[] = {"docs/new.md", "docs/old.md"};
  const char* carriedModified[] = {"src/older.cpp"};
  CHECK_ALL(PathsAre(&carriedLists.ReadFiles, carriedRead, 2),
            PathsAre(&carriedLists.ModifiedFiles, carriedModified, 1));

  StrBuf rendered = StrBuf_Make(arena, 256);
  StrBuf_AppendCStr(&rendered, "body");
  AgentCompaction_AppendFileSections(&rendered, &carriedLists);
  CHECK(StrView_EqCStr(StrBuf_View(&rendered),
                       "body\n\n<read-files>\ndocs/new.md\ndocs/old.md\n</read-files>"
                       "\n\n<modified-files>\nsrc/older.cpp\n</modified-files>"));

  // Nothing touched means nothing appended.
  AgentFileOps none = {};
  AgentFileLists empty = {};
  AgentCompaction_CollectFileOps(arena, nullptr, 0, DurableId{}, &none);
  AgentCompaction_BuildFileLists(arena, &none, &empty);
  StrBuf bare = StrBuf_Make(arena, 32);
  StrBuf_AppendCStr(&bare, "body");
  AgentCompaction_AppendFileSections(&bare, &empty);
  CHECK(StrView_EqCStr(StrBuf_View(&bare), "body"));

  // Malformed arguments are skipped rather than aborting collection.
  SessionRecord broken = MessageRecord(
      root.EntryId, AssistantToolCall(arena, SVL("c8"), SVL("read"), SVL("{\"path\":")));
  SessionRecord noPath = MessageRecord(
      broken.EntryId,
      AssistantToolCall(arena, SVL("c9"), SVL("read"), SVL("{\"limit\":10}")));
  SessionRecord good = MessageRecord(
      noPath.EntryId,
      AssistantToolCall(arena, SVL("c10"), SVL("read"), SVL("{\"path\":\"ok.txt\"}")));
  const SessionRecord* messy[] = {&root, &broken, &noPath, &good};
  AgentFileOps messyOps = {};
  AgentCompaction_CollectFileOps(arena, messy, 4, DurableId{}, &messyOps);
  AgentFileLists messyLists = {};
  AgentCompaction_BuildFileLists(arena, &messyOps, &messyLists);
  const char* messyRead[] = {"ok.txt"};
  CHECK(PathsAre(&messyLists.ReadFiles, messyRead, 1));
}

}  // namespace

void TestCompaction() {
  Arena arena = ArenaCreate((usize)8 << 20);
  TestEstimator(&arena);
  TestReportedTokens(&arena);
  TestShouldCompact();
  TestCutPoint(&arena);
  TestFileTracking(&arena);
  ArenaRelease(&arena);
}
