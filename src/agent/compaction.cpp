#include "agent/compaction.h"

#include "json/json.h"

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

int ComparePaths(StrView a, StrView b) {
  u32 shared = a.Length < b.Length ? a.Length : b.Length;
  for (u32 i = 0; i < shared; i++) {
    u8 left = (u8)a.Data[i];
    u8 right = (u8)b.Data[i];
    if (left != right) return left < right ? -1 : 1;
  }
  if (a.Length == b.Length) return 0;
  return a.Length < b.Length ? -1 : 1;
}

bool ContainsPath(const Array<StrView>* paths, StrView path) {
  for (u32 i = 0; i < paths->Count; i++)
    if (StrView_Eq(paths->Data[i], path)) return true;
  return false;
}

void AddPath(Arena* arena, Array<StrView>* paths, StrView path) {
  if (path.Length == 0 || ContainsPath(paths, path)) return;
  Array_Push(paths, StrView_Dup(arena, path));
}

void SortPaths(Array<StrView>* paths) {
  for (u32 i = 1; i < paths->Count; i++) {
    StrView value = paths->Data[i];
    u32 slot = i;
    while (slot > 0 && ComparePaths(paths->Data[slot - 1], value) > 0) {
      paths->Data[slot] = paths->Data[slot - 1];
      slot--;
    }
    paths->Data[slot] = value;
  }
}

// Which bucket a tool call contributes to, or null when the tool is not one of
// the three whose `path` argument is tracked.
Array<StrView>* BucketForTool(StrView toolName, AgentFileOps* ops) {
  if (StrView_EqCStr(toolName, "read")) return &ops->Read;
  if (StrView_EqCStr(toolName, "write")) return &ops->Written;
  if (StrView_EqCStr(toolName, "edit")) return &ops->Edited;
  return nullptr;
}

void CollectMessagePaths(Arena* arena, const Message* message, AgentFileOps* ops) {
  if (message->Kind != Role::Assistant) return;
  for (u32 i = 0; i < message->Content.Count; i++) {
    const Content* content = &message->Content.Data[i];
    if (content->Kind != ContentKind::ToolCall || content->ToolArgsJson.Length == 0)
      continue;
    Array<StrView>* bucket = BucketForTool(content->ToolName, ops);
    if (!bucket) continue;
    // The parser requires a null terminator one past the end, which a StrBuf
    // maintains but a view into the raw record does not.
    StrBuf text = StrBuf_Make(arena, content->ToolArgsJson.Length + 1);
    StrBuf_Append(&text, content->ToolArgsJson);
    JsonValue* args = Json_Parse(arena, StrBuf_View(&text));
    if (!Json_IsObject(args)) continue;
    AddPath(arena, bucket, Json_GetStr(args, "path"));
  }
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

void AgentCompaction_CollectFileOps(Arena* arena, const SessionRecord* const* branch,
                                    u32 branchCount, DurableId throughEntryId,
                                    AgentFileOps* out) {
  if (!arena || !out) return;
  out->Read = Array_Make<StrView>(arena, 8);
  out->Written = Array_Make<StrView>(arena, 8);
  out->Edited = Array_Make<StrView>(arena, 8);
  if (!branch) return;

  for (u32 i = 0; i < branchCount; i++) {
    const SessionRecord* record = branch[i];
    if (!record) continue;
    if (record->Kind == SessionRecordKind::Message) {
      CollectMessagePaths(arena, &record->MessageEntry.Value, out);
    } else if (record->Kind == SessionRecordKind::Compaction) {
      // An earlier compaction folded its own span away, so its stored lists are
      // the only surviving evidence of what those turns touched. Its modified
      // paths stay modified.
      const CompactionEntryData* entry = &record->CompactionEntry;
      for (u32 j = 0; j < entry->ReadFiles.Count; j++)
        AddPath(arena, &out->Read, entry->ReadFiles.Data[j]);
      for (u32 j = 0; j < entry->ModifiedFiles.Count; j++)
        AddPath(arena, &out->Edited, entry->ModifiedFiles.Data[j]);
    }
    if (DurableId_IsSet(throughEntryId) &&
        DurableId_Eq(record->EntryId, throughEntryId))
      break;
  }
}

void AgentCompaction_BuildFileLists(Arena* arena, const AgentFileOps* ops,
                                    AgentFileLists* out) {
  if (!arena || !ops || !out) return;
  u32 modifiedGuess = ops->Written.Count + ops->Edited.Count;
  out->ReadFiles = Array_Make<StrView>(arena, ops->Read.Count ? ops->Read.Count : 1);
  out->ModifiedFiles = Array_Make<StrView>(arena, modifiedGuess ? modifiedGuess : 1);

  for (u32 i = 0; i < ops->Written.Count; i++)
    AddPath(arena, &out->ModifiedFiles, ops->Written.Data[i]);
  for (u32 i = 0; i < ops->Edited.Count; i++)
    AddPath(arena, &out->ModifiedFiles, ops->Edited.Data[i]);
  for (u32 i = 0; i < ops->Read.Count; i++) {
    if (ContainsPath(&out->ModifiedFiles, ops->Read.Data[i])) continue;
    AddPath(arena, &out->ReadFiles, ops->Read.Data[i]);
  }

  SortPaths(&out->ReadFiles);
  SortPaths(&out->ModifiedFiles);
}

void AgentCompaction_AppendFileSections(StrBuf* out, const AgentFileLists* lists) {
  if (!out || !lists) return;
  struct Section {
    const char* Open;
    const char* Close;
    const Array<StrView>* Paths;
  };
  const Section sections[] = {
      {"<read-files>\n", "\n</read-files>", &lists->ReadFiles},
      {"<modified-files>\n", "\n</modified-files>", &lists->ModifiedFiles}};
  for (u32 s = 0; s < sizeof(sections) / sizeof(sections[0]); s++) {
    const Section& section = sections[s];
    if (!section.Paths->Count) continue;
    StrBuf_AppendCStr(out, "\n\n");
    StrBuf_AppendCStr(out, section.Open);
    for (u32 i = 0; i < section.Paths->Count; i++) {
      if (i) StrBuf_AppendChar(out, '\n');
      StrBuf_Append(out, section.Paths->Data[i]);
    }
    StrBuf_AppendCStr(out, section.Close);
  }
}
