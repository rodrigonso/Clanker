#include "session/reducer.h"

namespace {

SessionResult Invalid(const char* message) {
  return SessionResult_Error(SessionStatus::InvalidArgument, SV(message));
}

SessionResult Corrupt(const char* message) {
  return SessionResult_Error(SessionStatus::Corrupt, SV(message));
}

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

bool PendingContains(const SessionRecord* pending, u32 pendingCount,
                     DurableId id, bool treeOnly) {
  for (u32 i = 0; i < pendingCount; i++)
    if ((!treeOnly || SessionRecord_IsTreeNode(pending[i].Kind)) &&
        DurableId_Eq(pending[i].EntryId, id))
      return true;
  return false;
}

}  // namespace

SessionResult SessionReducer_ValidateAppend(const SessionReduction* reduction,
                                            const SessionRecord* candidate,
                                            const SessionRecord* pending,
                                            u32 pendingCount) {
  if (!reduction || !candidate) return Invalid("validate-append arguments are invalid");
  SessionResult self = SessionRecord_Validate(candidate);
  if (!SessionResult_IsOk(self)) return self;

  auto treeExists = [&](DurableId id) {
    u32 index = 0;
    bool historical =
        DurableIdIndex_Find(&reduction->Index, id, &index) &&
        SessionRecord_IsTreeNode(reduction->Records[index].Kind);
    return historical || PendingContains(pending, pendingCount, id, true);
  };

  u64 priorCount = (u64)reduction->RecordCount + pendingCount;
  if (candidate->Seq != priorCount + 1)
    return Invalid("seq must be contiguous with the existing log");

  if (reduction->RecordCount > 0 &&
      !DurableId_Eq(candidate->SessionId, reduction->Records[0].SessionId))
    return Invalid("sessionId must match the log it is appended to");
  if (reduction->RecordCount == 0 && pendingCount > 0 &&
      !DurableId_Eq(candidate->SessionId, pending[0].SessionId))
    return Invalid("sessionId must match the log it is appended to");

  if (DurableIdIndex_Contains(&reduction->Index, candidate->EntryId) ||
      PendingContains(pending, pendingCount, candidate->EntryId, false))
    return Invalid("entryId already exists in this session");

  if (candidate->Kind == SessionRecordKind::SessionStarted) {
    if (priorCount != 0) return Invalid("sessionStarted must be the first record");
    return SessionResult_Ok(candidate->Seq);
  }
  if (priorCount == 0) return Invalid("the first record must be sessionStarted");

  if (candidate->Kind == SessionRecordKind::SessionName) return SessionResult_Ok(candidate->Seq);

  if (candidate->Kind == SessionRecordKind::Leaf) {
    if (!treeExists(candidate->LeafEntry.LeafId))
      return Invalid("leaf target entryId was not found");
    return SessionResult_Ok(candidate->Seq);
  }

  DurableId parent = SessionRecord_ParentId(candidate);
  if (!treeExists(parent)) return Invalid("parentId was not found in this session");

  if (candidate->Kind == SessionRecordKind::Compaction) {
    if (!treeExists(candidate->CompactionEntry.BaseEntryId) ||
        !treeExists(candidate->CompactionEntry.ThroughEntryId) ||
        !treeExists(candidate->CompactionEntry.SourceHeadId))
      return Invalid("compaction references an entryId that was not found");
  }

  return SessionResult_Ok(candidate->Seq);
}

SessionResult SessionReducer_Ancestry(const SessionReduction* reduction, Arena* arena,
                                      DurableId headId,
                                      Array<const SessionRecord*>* outBranch) {
  if (!arena || !reduction || !outBranch || !DurableId_IsSet(headId))
    return Invalid("ancestry arguments are invalid");
  const SessionRecord* current = FindEntry(reduction, headId);
  if (!current) return SessionResult_Error(SessionStatus::NotFound, SVL("session head was not found"));

  Array<const SessionRecord*> reverse =
      Array_Make<const SessionRecord*>(arena, reduction->RecordCount ? reduction->RecordCount : 1);
  while (current) {
    Array_Push(&reverse, current);
    if (current->Kind == SessionRecordKind::SessionStarted) break;
    DurableId parent = SessionRecord_ParentId(current);
    current = FindEntry(reduction, parent);
    if (!current) return Corrupt("branch parent was not found");
  }
  if (reverse.Count == 0 ||
      reverse.Data[reverse.Count - 1]->Kind != SessionRecordKind::SessionStarted)
    return Corrupt("branch does not reach the session root");

  Array<const SessionRecord*> branch =
      Array_Make<const SessionRecord*>(arena, reverse.Count);
  for (u32 i = reverse.Count; i > 0; i--) Array_Push(&branch, reverse.Data[i - 1]);
  *outBranch = branch;
  return SessionResult_Ok();
}

SessionResult SessionReducer_Context(Arena* arena, const SessionReduction* reduction,
                                     DurableId headId, Array<Message>* outMessages) {
  if (!arena || !reduction || !outMessages) return Invalid("context arguments are invalid");
  Array<const SessionRecord*> branch = {};
  SessionResult ancestry = SessionReducer_Ancestry(reduction, arena, headId, &branch);
  if (!SessionResult_IsOk(ancestry)) return ancestry;

  i32 compactionIndex = -1;
  for (u32 i = 0; i < branch.Count; i++)
    if (branch.Data[i]->Kind == SessionRecordKind::Compaction) compactionIndex = (i32)i;

  Array<Message> messages = Array_Make<Message>(arena, branch.Count ? branch.Count : 1);
  u32 start = 0;
  if (compactionIndex >= 0) {
    Array_Push(&messages, branch.Data[compactionIndex]->CompactionEntry.Summary);
    start = (u32)compactionIndex + 1;
  }
  for (u32 i = start; i < branch.Count; i++) {
    if (branch.Data[i]->Kind == SessionRecordKind::Message)
      Array_Push(&messages, branch.Data[i]->MessageEntry.Value);
  }
  *outMessages = messages;
  return SessionResult_Ok();
}

SessionResult SessionReducer_Reduce(Arena* arena, const SessionRecord* records, u32 count,
                                    SessionReduction* out) {
  if (!arena || !records || count == 0 || !out) return Invalid("reducer arguments are invalid");
  SessionResult valid = SessionRecords_Validate(records, count);
  if (!SessionResult_IsOk(valid)) return valid;

  SessionReduction result = {};
  DurableIdIndex_Init(&result.Index, arena, count);
  for (u32 i = 0; i < count; i++) SessionReducer_Fold(&result, records, i + 1);

  *out = result;
  return SessionResult_Ok(records[count - 1].Seq);
}

SessionResult SessionRecords_Validate(const SessionRecord* records, u32 count) {
  if (!records || count == 0) return Invalid("records are required");
  if (records[0].Kind != SessionRecordKind::SessionStarted)
    return Corrupt("the first record must be sessionStarted");

  if (count > (1u << 30))
    return Corrupt("session contains too many records");
  usize indexBytes = DurableIdIndex_AllocationSize(count);
  Arena scratch = ArenaCreate(indexBytes + ((usize)64 << 10));
  DurableIdIndex index;
  DurableIdIndex_Init(&index, &scratch, count);
  DurableId sessionId = records[0].SessionId;
  DurableId headId = {};
  bool headIsSet = false;

  SessionResult result = SessionResult_Ok();
  for (u32 i = 0; i < count; i++) {
    const SessionRecord* record = &records[i];
    SessionResult self = SessionRecord_Validate(record);
    if (!SessionResult_IsOk(self)) {
      result = self;
      break;
    }
    if (!DurableId_Eq(record->SessionId, sessionId)) {
      result = Corrupt("all records must share the same sessionId");
      break;
    }
    if (record->Seq != (u64)i + 1) {
      result = Corrupt("seq must be contiguous starting at 1");
      break;
    }
    if (DurableIdIndex_Contains(&index, record->EntryId)) {
      result = Corrupt("duplicate entryId in session log");
      break;
    }

    auto treeExists = [&](DurableId id) {
      u32 recordIndex = 0;
      return DurableIdIndex_Find(&index, id, &recordIndex) &&
             SessionRecord_IsTreeNode(records[recordIndex].Kind);
    };

    if (record->Kind == SessionRecordKind::SessionStarted) {
      if (i != 0) {
        result = Corrupt("sessionStarted must be the first record");
        break;
      }
      headId = record->EntryId;
      headIsSet = true;
    } else if (record->Kind == SessionRecordKind::SessionName) {
      // no parent, does not affect head
    } else if (record->Kind == SessionRecordKind::Leaf) {
      if (!treeExists(record->LeafEntry.LeafId)) {
        result = Corrupt("leaf target entryId was not found");
        break;
      }
      headId = record->LeafEntry.LeafId;
      headIsSet = true;
    } else {
      DurableId parent = SessionRecord_ParentId(record);
      if (!treeExists(parent)) {
        result = Corrupt("parentId was not found before its reference");
        break;
      }
      if (record->Kind == SessionRecordKind::Compaction) {
        if (!treeExists(record->CompactionEntry.BaseEntryId) ||
            !treeExists(record->CompactionEntry.ThroughEntryId) ||
            !treeExists(record->CompactionEntry.SourceHeadId)) {
          result = Corrupt("compaction references an entryId that was not found");
          break;
        }
      }
      if (headIsSet && DurableId_Eq(parent, headId)) {
        headId = record->EntryId;
        headIsSet = true;
      }
    }

    DurableIdIndex_Put(&index, record->EntryId, i);
  }
  ArenaRelease(&scratch);
  return result;
}
