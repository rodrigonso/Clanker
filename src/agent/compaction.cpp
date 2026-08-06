#include "agent/compaction.h"

namespace {

u32 MessageContentBytes(const Message* message) {
  u32 bytes = 0;
  for (u32 i = 0; i < message->Content.Count; i++) {
    const Content* content = &message->Content.Data[i];
    bytes = AddSaturated(bytes, content->Text.Length);
    bytes = AddSaturated(bytes, content->ToolName.Length);
    bytes = AddSaturated(bytes, content->ToolArgsJson.Length);
    bytes = AddSaturated(bytes, content->ImageData.Length);
  }
  bytes = AddSaturated(bytes, message->ToolName.Length);
  bytes = AddSaturated(bytes, message->ErrorMessage.Length);
  return bytes;
}

}  // namespace

bool AgentCompactionPolicy_IsValid(const AgentCompactionPolicy* policy) {
  return policy && policy->Enabled && policy->ReserveTokens &&
         policy->KeepRecentTokens;
}

u32 AgentCompaction_EstimateMessageTokens(const Message* message) {
  if (!message) return 0;
  // Per-message envelope overhead (role, delimiters, tool wiring) that every
  // provider pays regardless of content length.
  constexpr u32 kMessageOverheadTokens = 4;
  u32 bytes = MessageContentBytes(message);
  return AddSaturated(bytes / AGENT_COMPACTION_BYTES_PER_TOKEN,
                      kMessageOverheadTokens);
}

u32 AgentCompaction_EstimateContextTokens(const Message* messages, u32 count) {
  if (!messages) return 0;
  u32 total = 0;
  for (u32 i = 0; i < count; i++)
    total = AddSaturated(total, AgentCompaction_EstimateMessageTokens(&messages[i]));
  return total;
}

u32 AgentCompaction_ReportedContextTokens(const Message* messages, u32 count) {
  if (!messages || count == 0) return 0;

  // Newest assistant message that actually reported usage. Everything after it
  // has not been billed yet, so it is estimated.
  u32 anchor = count;
  for (u32 i = count; i > 0; i--) {
    const Message* message = &messages[i - 1];
    if (message->Kind != Role::Assistant) continue;
    const Usage* use = &message->Use;
    if (use->Input || use->CacheRead || use->CacheWrite || use->Output) {
      anchor = i - 1;
      break;
    }
  }
  if (anchor == count) return 0;

  const Usage* use = &messages[anchor].Use;
  u32 total = use->Input;
  total = AddSaturated(total, use->CacheRead);
  total = AddSaturated(total, use->CacheWrite);
  total = AddSaturated(total, use->Output);
  for (u32 i = anchor + 1; i < count; i++)
    total = AddSaturated(total, AgentCompaction_EstimateMessageTokens(&messages[i]));
  return total;
}

u32 AgentCompaction_ContextTokens(const Message* messages, u32 count) {
  u32 reported = AgentCompaction_ReportedContextTokens(messages, count);
  return reported ? reported : AgentCompaction_EstimateContextTokens(messages, count);
}

bool AgentCompaction_ShouldCompact(const AgentCompactionPolicy* policy,
                                   u32 contextWindow, u32 contextTokens) {
  if (!AgentCompactionPolicy_IsValid(policy) || contextWindow == 0) return false;
  if (policy->ReserveTokens >= contextWindow) return false;
  return contextTokens > contextWindow - policy->ReserveTokens;
}

bool AgentCompaction_SelectCutPoint(const SessionRecord* const* branch, u32 branchCount,
                                    u32 keepRecentTokens, AgentCompactionCut* out) {
  if (!branch || branchCount == 0 || !out) return false;

  u32 firstMessage = branchCount;
  u32 lastMessage = branchCount;
  for (u32 i = 0; i < branchCount; i++) {
    if (!branch[i] || branch[i]->Kind != SessionRecordKind::Message) continue;
    if (firstMessage == branchCount) firstMessage = i;
    lastMessage = i;
  }
  if (firstMessage == branchCount) return false;

  // Walk backwards accumulating the retained tail, remembering the oldest user
  // boundary that still fits the budget. Everything before it gets folded away.
  u32 tailStart = branchCount;
  u32 tailTokens = 0;
  for (u32 i = lastMessage + 1; i > firstMessage; i--) {
    u32 index = i - 1;
    const SessionRecord* record = branch[index];
    if (!record || record->Kind != SessionRecordKind::Message) continue;
    const Message* message = &record->MessageEntry.Value;
    u32 cost = AgentCompaction_EstimateMessageTokens(message);
    if (tailTokens + cost > keepRecentTokens) break;
    tailTokens += cost;
    if (message->Kind == Role::User) tailStart = index;
  }

  AgentCompactionCut cut = {};
  cut.BaseEntryId = branch[0]->EntryId;

  // The whole message range fits the budget: there is nothing to fold away.
  if (tailStart == firstMessage) return false;

  if (tailStart == branchCount) {
    // No user boundary fits the budget. Fold the whole branch, matching the
    // pre-retention behaviour; pi's split-turn summary is not implemented.
    cut.ThroughEntryId = branch[lastMessage]->EntryId;
    cut.HasRetainedTail = false;
  } else {
    cut.ThroughEntryId = branch[tailStart - 1]->EntryId;
    cut.HasRetainedTail = true;
  }
  *out = cut;
  return true;
}
