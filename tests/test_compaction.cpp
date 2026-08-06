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
  use.CacheWrite = 5;
  use.Output = 20;
  Message reported[2] = {UserMessage(arena, SVL("aaaa")),
                         AssistantText(arena, SVL("dddd"), use)};
  CHECK(AgentCompaction_ReportedContextTokens(reported, 2) == 135);
  CHECK(AgentCompaction_ContextTokens(reported, 2) == 135);

  // Messages appended after that assistant turn have not been billed, so they
  // are estimated on top.
  Message trailing[3] = {
      UserMessage(arena, SVL("aaaa")), AssistantText(arena, SVL("dddd"), use),
      Message_ToolResult(arena, SVL("call-1"), SVL("t"), SVL("bbbb"), false)};
  CHECK(AgentCompaction_ReportedContextTokens(trailing, 3) == 140);
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

}  // namespace

void TestCompaction() {
  Arena arena = ArenaCreate((usize)8 << 20);
  TestEstimator(&arena);
  TestReportedTokens(&arena);
  TestShouldCompact();
  TestCutPoint(&arena);
  ArenaRelease(&arena);
}
