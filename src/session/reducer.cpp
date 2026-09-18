#include "session/reducer.h"

namespace {

const SessionRecord* FindEntry(const SessionReduction* reduction, DurableId id) {
  u32 index;
  if (!DurableIdIndex_Find(&reduction->Index, id, &index)) return nullptr;
  const SessionRecord* record = &reduction->Records[index];
  return SessionRecord_IsTreeNode(record->Kind) ? record : nullptr;
}

// Walks backward from `reduction->HeadId` to the session root, recomputing
// the branch-scoped Provider/Model/ThinkingLevel/ToolNames from whichever
// Model/Thinking/Tools record is nearest to the head. O(branch); only called
// when a Leaf record explicitly relocates the head (checkout), never on the
// ordinary append-onto-head path.
void RecomputeBranchSettings(SessionReduction* reduction) {
  reduction->Provider = StrView{};
  reduction->Model = StrView{};
  reduction->ThinkingLevel = StrView{};
  reduction->ToolNames = Array<StrView>{};
  bool haveModel = false;
  bool haveThinking = false;
  bool haveTools = false;

  DurableId current = reduction->HeadId;
  while (DurableId_IsSet(current)) {
    const SessionRecord* record = FindEntry(reduction, current);
    if (!record) break;
    if (!haveModel && record->Kind == SessionRecordKind::Model) {
      reduction->Provider = record->ModelEntry.Provider;
      reduction->Model = record->ModelEntry.Model;
      haveModel = true;
    } else if (!haveThinking && record->Kind == SessionRecordKind::Thinking) {
      reduction->ThinkingLevel = record->ThinkingEntry.Level;
      haveThinking = true;
    } else if (!haveTools && record->Kind == SessionRecordKind::Tools) {
      reduction->ToolNames = record->ToolsEntry.ToolNames;
      haveTools = true;
    }
    if (record->Kind == SessionRecordKind::SessionStarted) break;
    current = SessionRecord_ParentId(record);
  }
}

}  // namespace

void SessionReducer_Fold(SessionReduction* reduction, const SessionRecord* records, u32 count) {
  reduction->Records = records;
  reduction->RecordCount = count;
  if (count == 0) return;
  u32 i = count - 1;
  const SessionRecord* record = &records[i];

  DurableIdIndex_Put(&reduction->Index, record->EntryId, i);

  if (record->Kind == SessionRecordKind::SessionName) return;

  if (record->Kind == SessionRecordKind::Leaf) {
    reduction->HeadId = record->LeafEntry.LeafId;
    reduction->HeadIsSet = true;
    RecomputeBranchSettings(reduction);
    return;
  }

  if (record->Kind == SessionRecordKind::SessionStarted) {
    reduction->HeadId = record->EntryId;
    reduction->HeadIsSet = true;
    return;
  }

  DurableId parent = SessionRecord_ParentId(record);
  bool chainsOntoHead = reduction->HeadIsSet && DurableId_Eq(parent, reduction->HeadId);
  if (!chainsOntoHead) return;  // appended onto an inactive branch; indexed, not the new head

  reduction->HeadId = record->EntryId;
  reduction->HeadIsSet = true;
  switch (record->Kind) {
    case SessionRecordKind::Model:
      reduction->Provider = record->ModelEntry.Provider;
      reduction->Model = record->ModelEntry.Model;
      break;
    case SessionRecordKind::Thinking:
      reduction->ThinkingLevel = record->ThinkingEntry.Level;
      break;
    case SessionRecordKind::Tools:
      reduction->ToolNames = record->ToolsEntry.ToolNames;
      break;
    default: break;
  }
}

namespace {

const SessionRecord* FindPending(const SessionRecord* pending, u32 pendingCount,
                                 DurableId id) {
  for (u32 i = 0; i < pendingCount; i++)
    if (DurableId_Eq(pending[i].EntryId, id)) return &pending[i];
  return nullptr;
}

}  // namespace

SessionResult SessionReducer_ValidateAppend(const SessionReduction* reduction,
                                            const SessionRecord* candidate,
                                            const SessionRecord* pending,
                                            u32 pendingCount) {
  if (!reduction || !candidate || (!pending && pendingCount))
    return SessionResult_Invalid("validate-append arguments are invalid");
  SessionResult self = SessionRecord_Validate(candidate);
  if (!SessionResult_IsOk(self)) return self;

  auto findTree = [&](DurableId id) -> const SessionRecord* {
    const SessionRecord* found = FindEntry(reduction, id);
    if (!found) found = FindPending(pending, pendingCount, id);
    return found && SessionRecord_IsTreeNode(found->Kind) ? found : nullptr;
  };

  u64 priorCount = (u64)reduction->RecordCount + pendingCount;
  if (candidate->Seq != priorCount + 1)
    return SessionResult_Invalid("seq must be contiguous with the existing log");

  if (reduction->RecordCount > 0 &&
      !DurableId_Eq(candidate->SessionId, reduction->Records[0].SessionId))
    return SessionResult_Invalid("sessionId must match the log it is appended to");
  if (reduction->RecordCount == 0 && pendingCount > 0 &&
      !DurableId_Eq(candidate->SessionId, pending[0].SessionId))
    return SessionResult_Invalid("sessionId must match the log it is appended to");

  if (DurableIdIndex_Contains(&reduction->Index, candidate->EntryId) ||
      FindPending(pending, pendingCount, candidate->EntryId))
    return SessionResult_Invalid("entryId already exists in this session");

  if (candidate->Kind == SessionRecordKind::SessionStarted) {
    if (priorCount != 0) return SessionResult_Invalid("sessionStarted must be the first record");
    return SessionResult_Ok(candidate->Seq);
  }
  if (priorCount == 0) return SessionResult_Invalid("the first record must be sessionStarted");

  if (candidate->Kind == SessionRecordKind::SessionName) return SessionResult_Ok(candidate->Seq);

  if (candidate->Kind == SessionRecordKind::Leaf) {
    if (!findTree(candidate->LeafEntry.LeafId))
      return SessionResult_Invalid("leaf target entryId was not found");
    return SessionResult_Ok(candidate->Seq);
  }

  DurableId parent = SessionRecord_ParentId(candidate);
  const SessionRecord* parentRecord = findTree(parent);
  if (!parentRecord) return SessionResult_Invalid("parentId was not found in this session");

  if (candidate->Kind == SessionRecordKind::Compaction) {
    const CompactionEntryData* compaction = &candidate->CompactionEntry;
    const SessionRecord* base = findTree(compaction->BaseEntryId);
    const SessionRecord* through = findTree(compaction->ThroughEntryId);
    if (!base || !through)
      return SessionResult_Invalid("compaction references an entryId that was not found");
    if (!DurableId_Eq(compaction->SourceHeadId, parent))
      return SessionResult_Invalid("compaction sourceHeadId must equal parentId");
    if (base->Seq > through->Seq)
      return SessionResult_Invalid("compaction span ends before it begins");

    // Earlier records already have valid parents. Walking the candidate's
    // ancestry, not the active head, admits inactive branches and pending
    // batch nodes without allocating or scanning unrelated history.
    const SessionRecord* current = parentRecord;
    bool foundThrough = false;
    while (current && current->Seq >= base->Seq) {
      if (current == through) foundThrough = true;
      if (current == base) {
        if (foundThrough) return SessionResult_Ok(candidate->Seq);
        break;
      }
      current = findTree(SessionRecord_ParentId(current));
    }
    return SessionResult_Invalid("compaction span is not on its parent's branch");
  }

  return SessionResult_Ok(candidate->Seq);
}

SessionResult SessionReducer_Ancestry(const SessionReduction* reduction, Arena* arena,
                                      DurableId headId,
                                      Array<const SessionRecord*>* outBranch) {
  if (!arena || !reduction || !outBranch || !DurableId_IsSet(headId))
    return SessionResult_Invalid("ancestry arguments are invalid");
  const SessionRecord* current = FindEntry(reduction, headId);
  if (!current) return SessionResult_Error(SessionStatus::NotFound, SVL("session head was not found"));

  Array<const SessionRecord*> branch = Array_Make<const SessionRecord*>(arena);
  while (current) {
    Array_Push(&branch, current);
    if (current->Kind == SessionRecordKind::SessionStarted) break;
    DurableId parent = SessionRecord_ParentId(current);
    current = FindEntry(reduction, parent);
    if (!current) return SessionResult_Corrupt("branch parent was not found");
  }
  if (branch.Count == 0 ||
      branch.Data[branch.Count - 1]->Kind != SessionRecordKind::SessionStarted)
    return SessionResult_Corrupt("branch does not reach the session root");

  for (u32 i = 0; i < branch.Count / 2; i++) {
    u32 opposite = branch.Count - i - 1;
    const SessionRecord* record = branch.Data[i];
    branch.Data[i] = branch.Data[opposite];
    branch.Data[opposite] = record;
  }
  *outBranch = branch;
  return SessionResult_Ok();
}

// How one branch entry contributes to the materialized context.
enum class ContextSlot : u8 {
  Own,        // emit the entry's own message, when it has one
  Suppressed, // folded into some compaction's summary
  Summary,    // anchor position for a compaction's summary
};

SessionResult SessionReducer_Context(Arena* arena, const SessionReduction* reduction,
                                     DurableId headId, Array<Message>* outMessages) {
  return SessionReducer_ContextThrough(arena, reduction, headId, DurableId{}, outMessages);
}

SessionResult SessionReducer_ContextThrough(Arena* arena, const SessionReduction* reduction,
                                            DurableId headId, DurableId throughId,
                                            Array<Message>* outMessages) {
  if (!arena || !reduction || !outMessages) return SessionResult_Invalid("context arguments are invalid");
  Array<const SessionRecord*> branch = {};
  SessionResult ancestry = SessionReducer_Ancestry(reduction, arena, headId, &branch);
  if (!SessionResult_IsOk(ancestry)) return ancestry;

  u32 limit = branch.Count;
  if (DurableId_IsSet(throughId)) {
    limit = 0;
    for (u32 i = 0; i < branch.Count && !limit; i++)
      if (DurableId_Eq(branch.Data[i]->EntryId, throughId)) limit = i + 1;
    if (!limit) return SessionResult_Invalid("context limit is not on the branch");
  }

  u32 compactionCount = 0;
  for (u32 i = 0; i < branch.Count; i++)
    if (branch.Data[i]->Kind == SessionRecordKind::Compaction) compactionCount++;

  Array<Message> messages = Array_Make<Message>(arena, limit ? limit : 1);

  if (compactionCount == 0) {
    for (u32 i = 0; i < limit; i++) {
      if (branch.Data[i]->Kind == SessionRecordKind::Message)
        Array_Push(&messages, branch.Data[i]->MessageEntry.Value);
    }
    *outMessages = messages;
    return SessionResult_Ok();
  }

  // Each compaction folds the inclusive branch span [Base..Through] into a
  // single summary anchored at Base. Applying them in branch order means a
  // later compaction that spans an earlier anchor correctly supersedes it.
  DurableIdIndex position = {};
  DurableIdIndex_Init(&position, arena, branch.Count);
  for (u32 i = 0; i < branch.Count; i++)
    DurableIdIndex_Put(&position, branch.Data[i]->EntryId, i);

  ContextSlot* slot = ArenaPushArray(arena, ContextSlot, branch.Count);
  const Message** anchor = ArenaPushArray(arena, const Message*, branch.Count);
  for (u32 i = 0; i < branch.Count; i++) {
    slot[i] = ContextSlot::Own;
    anchor[i] = nullptr;
  }

  for (u32 i = 0; i < branch.Count; i++) {
    const SessionRecord* record = branch.Data[i];
    if (record->Kind != SessionRecordKind::Compaction) continue;
    u32 base = 0;
    u32 through = 0;
    if (!DurableIdIndex_Find(&position, record->CompactionEntry.BaseEntryId, &base) ||
        !DurableIdIndex_Find(&position, record->CompactionEntry.ThroughEntryId, &through))
      return SessionResult_Corrupt("compaction span references an entry that is not on this branch");
    if (base > through) return SessionResult_Corrupt("compaction span ends before it begins");
    if (through >= i) return SessionResult_Corrupt("compaction span reaches its own entry or later");
    for (u32 k = base; k <= through; k++) {
      slot[k] = ContextSlot::Suppressed;
      anchor[k] = nullptr;
    }
    slot[base] = ContextSlot::Summary;
    anchor[base] = &record->CompactionEntry.Summary;
  }

  for (u32 i = 0; i < limit; i++) {
    if (slot[i] == ContextSlot::Summary) {
      Array_Push(&messages, *anchor[i]);
      continue;
    }
    if (slot[i] == ContextSlot::Suppressed) continue;
    if (branch.Data[i]->Kind == SessionRecordKind::Message)
      Array_Push(&messages, branch.Data[i]->MessageEntry.Value);
  }
  *outMessages = messages;
  return SessionResult_Ok();
}

SessionResult SessionReducer_Reduce(Arena* arena, const SessionRecord* records, u32 count,
                                    SessionReduction* out) {
  if (!arena || !records || count == 0 || !out) return SessionResult_Invalid("reducer arguments are invalid");
  SessionResult valid = SessionRecords_Validate(records, count);
  if (!SessionResult_IsOk(valid)) return valid;

  SessionReduction result = {};
  DurableIdIndex_Init(&result.Index, arena, count);
  for (u32 i = 0; i < count; i++) SessionReducer_Fold(&result, records, i + 1);

  *out = result;
  return SessionResult_Ok(records[count - 1].Seq);
}

SessionResult SessionRecords_Validate(const SessionRecord* records, u32 count) {
  if (!records || count == 0) return SessionResult_Invalid("records are required");
  if (count > (1u << 30))
    return SessionResult_Corrupt("session contains too many records");
  usize indexBytes = DurableIdIndex_AllocationSize(count);
  Arena scratch = ArenaCreate(indexBytes + ((usize)64 << 10));
  SessionReduction prefix = {};
  prefix.Records = records;
  DurableIdIndex_Init(&prefix.Index, &scratch, count);

  SessionResult result = SessionResult_Ok();
  for (u32 i = 0; i < count; i++) {
    result = SessionReducer_ValidateAppend(&prefix, &records[i]);
    if (!SessionResult_IsOk(result)) {
      if (result.Status != SessionStatus::UnsupportedVersion)
        result.Status = SessionStatus::Corrupt;
      break;
    }
    DurableIdIndex_Put(&prefix.Index, records[i].EntryId, i);
    prefix.RecordCount = i + 1;
  }
  ArenaRelease(&scratch);
  return result;
}
