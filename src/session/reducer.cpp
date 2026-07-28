#include "session/reducer.h"

namespace {

DurableId ParentId(const SessionRecord* record) {
  switch (record->Kind) {
    case SessionRecordKind::Message: return record->MessageEntry.ParentId;
    case SessionRecordKind::Model: return record->ModelEntry.ParentId;
    case SessionRecordKind::Thinking: return record->ThinkingEntry.ParentId;
    case SessionRecordKind::Tools: return record->ToolsEntry.ParentId;
    case SessionRecordKind::Compaction: return record->CompactionEntry.ParentId;
    case SessionRecordKind::BranchLabel: return record->BranchLabelEntry.ParentId;
    default: return DurableId{};
  }
}

const SessionRecord* FindEntry(const SessionReduction* reduction, DurableId id) {
  for (u32 i = 0; i < reduction->Nodes.Count; i++) {
    if (DurableId_Eq(reduction->Nodes.Data[i].EntryId, id))
      return &reduction->Records[reduction->Nodes.Data[i].RecordIndex];
  }
  return nullptr;
}

bool IdArrayContains(const Array<DurableId>* ids, DurableId id) {
  for (u32 i = 0; i < ids->Count; i++)
    if (DurableId_Eq(ids->Data[i], id)) return true;
  return false;
}

bool QueueItemConsumed(const SessionRecord* records, u32 count, u32 acceptedIndex,
                       DurableId itemId) {
  for (u32 i = acceptedIndex + 1; i < count; i++) {
    if (records[i].Kind == SessionRecordKind::QueueCheckpoint &&
        IdArrayContains(&records[i].QueueCheckpoint.ItemIds, itemId))
      return true;
  }
  return false;
}

bool DeferredWriteCommitted(const SessionRecord* records, u32 count, u32 deferredIndex,
                            DurableId deferredId) {
  for (u32 i = deferredIndex + 1; i < count; i++) {
    if (records[i].Kind == SessionRecordKind::DeferredWriteCommitted &&
        DurableId_Eq(records[i].DeferredWriteCommitted.DeferredId, deferredId))
      return true;
  }
  return false;
}

bool GenerationFinished(const SessionRecord* records, u32 count, u32 startedIndex,
                        DurableId attemptId) {
  for (u32 i = startedIndex + 1; i < count; i++) {
    if (records[i].Kind == SessionRecordKind::GenerationFinished &&
        DurableId_Eq(records[i].GenerationFinished.AttemptId, attemptId))
      return true;
  }
  return false;
}

bool ToolFinished(const SessionRecord* records, u32 count, u32 startedIndex,
                  DurableId executionId) {
  for (u32 i = startedIndex + 1; i < count; i++) {
    if (records[i].Kind == SessionRecordKind::ToolFinished &&
        DurableId_Eq(records[i].ToolFinished.ToolExecutionId, executionId))
      return true;
  }
  return false;
}

const SessionRecord* LatestRecovery(const SessionRecord* records, u32 count,
                                    u32 startedIndex, DurableId executionId) {
  const SessionRecord* result = nullptr;
  for (u32 i = startedIndex + 1; i < count; i++) {
    if (records[i].Kind == SessionRecordKind::RecoveryResolved &&
        DurableId_Eq(records[i].RecoveryResolved.ToolExecutionId, executionId))
      result = &records[i];
  }
  return result;
}

SessionResult Corrupt(const char* message) {
  return SessionResult_Error(SessionStatus::Corrupt, SV(message));
}

}  // namespace

SessionResult SessionReducer_Ancestry(Arena* arena, const SessionReduction* reduction,
                                      DurableId headId,
                                      Array<const SessionRecord*>* outBranch) {
  if (!arena || !reduction || !outBranch || !DurableId_IsSet(headId))
    return SessionResult_Error(SessionStatus::InvalidArgument,
                               SVL("ancestry arguments are invalid"));
  const SessionRecord* current = FindEntry(reduction, headId);
  if (!current || current->EntryClass != SessionEntryClass::Session)
    return SessionResult_Error(SessionStatus::NotFound, SVL("session head was not found"));

  Array<const SessionRecord*> reverse =
      Array_Make<const SessionRecord*>(arena, reduction->Nodes.Count);
  while (current) {
    Array_Push(&reverse, current);
    if (current->Kind == SessionRecordKind::SessionStarted) break;
    DurableId parent = ParentId(current);
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
  if (!arena || !reduction || !outMessages)
    return SessionResult_Error(SessionStatus::InvalidArgument,
                               SVL("context arguments are invalid"));
  Array<const SessionRecord*> branch = {};
  SessionResult ancestry = SessionReducer_Ancestry(arena, reduction, headId, &branch);
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
  if (!arena || !records || count == 0 || !out)
    return SessionResult_Error(SessionStatus::InvalidArgument,
                               SVL("reducer arguments are invalid"));
  SessionResult valid = SessionRecords_Validate(records, count);
  if (!SessionResult_IsOk(valid)) return valid;

  SessionReduction result = {};
  result.Records = records;
  result.RecordCount = count;
  result.Nodes = Array_Make<SessionNode>(arena, count);
  result.PendingTools = Array_Make<const SessionRecord*>(arena, 4);
  result.PendingQueueItems = Array_Make<const SessionRecord*>(arena, 8);
  result.PendingDeferredWrites = Array_Make<const SessionRecord*>(arena, 4);
  result.HeadId = records[0].EntryId;

  for (u32 i = 0; i < count; i++) {
    const SessionRecord* record = &records[i];
    if (record->EntryClass == SessionEntryClass::Session) {
      SessionNode node = {};
      node.EntryId = record->EntryId;
      node.ParentId = ParentId(record);
      node.RecordIndex = i;
      Array_Push(&result.Nodes, node);
      if (record->Kind != SessionRecordKind::SessionStarted &&
          DurableId_Eq(node.ParentId, result.HeadId))
        result.HeadId = record->EntryId;
    }

    switch (record->Kind) {
      case SessionRecordKind::OperationStarted:
        result.HasActiveOperation = true;
        result.ActiveOperationStart = record;
        result.ActiveOperationId = record->OperationStarted.OperationId;
        result.ActiveOperationKind = record->OperationStarted.Operation;
        result.CancelRequested = false;
        break;
      case SessionRecordKind::OperationCancelRequested:
        result.CancelRequested = true;
        break;
      case SessionRecordKind::OperationFinished:
        if (record->OperationFinished.Outcome == OperationOutcome::Succeeded &&
            DurableId_IsSet(record->OperationFinished.ResultHeadId))
          result.HeadId = record->OperationFinished.ResultHeadId;
        result.HasActiveOperation = false;
        result.ActiveOperationStart = nullptr;
        result.ActiveOperationId = DurableId{};
        result.CancelRequested = false;
        break;
      case SessionRecordKind::OperationAborted:
        result.HasActiveOperation = false;
        result.ActiveOperationStart = nullptr;
        result.ActiveOperationId = DurableId{};
        result.CancelRequested = false;
        break;
      default: break;
    }
  }

  SessionResult ancestry =
      SessionReducer_Ancestry(arena, &result, result.HeadId, &result.Branch);
  if (!SessionResult_IsOk(ancestry)) return ancestry;
  for (u32 i = 0; i < result.Branch.Count; i++) {
    const SessionRecord* record = result.Branch.Data[i];
    if (record->Kind == SessionRecordKind::Model) {
      result.Provider = record->ModelEntry.Provider;
      result.Model = record->ModelEntry.Model;
    } else if (record->Kind == SessionRecordKind::Thinking) {
      result.ThinkingLevel = record->ThinkingEntry.Level;
    } else if (record->Kind == SessionRecordKind::Tools) {
      result.ToolNames = record->ToolsEntry.ToolNames;
    }
  }

  if (result.HasActiveOperation) {
    bool pendingGeneration = false;
    bool pendingReplaySafe = false;
    bool pendingManual = false;
    for (u32 i = 0; i < count; i++) {
      const SessionRecord* record = &records[i];
      if (record->Kind == SessionRecordKind::GenerationStarted &&
          DurableId_Eq(record->GenerationStarted.OperationId,
                       result.ActiveOperationId) &&
          !GenerationFinished(records, count, i, record->GenerationStarted.AttemptId)) {
        pendingGeneration = true;
        result.PendingAttemptId = record->GenerationStarted.AttemptId;
        result.PendingGeneration = record;
      } else if (record->Kind == SessionRecordKind::ToolStarted &&
                 DurableId_Eq(record->ToolStarted.OperationId,
                              result.ActiveOperationId) &&
                 !ToolFinished(records, count, i, record->ToolStarted.ToolExecutionId)) {
        Array_Push(&result.PendingTools, record);
        const SessionRecord* recovery =
            LatestRecovery(records, count, i, record->ToolStarted.ToolExecutionId);
        const SessionRecord* durableResult =
            FindEntry(&result, record->ToolStarted.ProvisionedResultEntryId);
        if (durableResult ||
            record->ToolStarted.ReplayPolicy == ToolReplayPolicy::ReplaySafe ||
            recovery)
          pendingReplaySafe = true;
        else
          pendingManual = true;
      }
    }
    if (pendingManual)
      result.Suspension = SessionSuspensionKind::ManualTool;
    else if (pendingReplaySafe)
      result.Suspension = SessionSuspensionKind::ReplaySafeTool;
    else if (pendingGeneration)
      result.Suspension = SessionSuspensionKind::Generation;
    else
      result.Suspension = SessionSuspensionKind::Operation;
  }

  for (u32 i = 0; i < count; i++) {
    const SessionRecord* record = &records[i];
    if (record->Kind == SessionRecordKind::QueueItemAccepted &&
        !QueueItemConsumed(records, count, i, record->QueueItemAccepted.ItemId))
      Array_Push(&result.PendingQueueItems, record);
    if (record->Kind == SessionRecordKind::DeferredWrite &&
        !DeferredWriteCommitted(records, count, i, record->DeferredWrite.DeferredId))
      Array_Push(&result.PendingDeferredWrites, record);
  }

  *out = result;
  return SessionResult_Ok(records[count - 1].Seq);
}
