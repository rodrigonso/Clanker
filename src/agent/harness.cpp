#include "agent/harness.h"

#include "agent/driver.h"

namespace {

SessionResult Invalid(const char* message) {
  return SessionResult_Error(SessionStatus::InvalidArgument, SV(message));
}

SessionResult HookFailure(SessionResult result, const char* fallback) {
  if (SessionResult_IsOk(result)) return result;
  return SessionResult_Error(
      SessionStatus::HookError,
      result.Message.Length ? result.Message : SV(fallback), result.Seq);
}

SessionResult QueueMessage(AgentHarness* harness, QueueKind queue, StrView text) {
  if (!harness || !harness->Initialized || text.Length == 0)
    return Invalid("queue arguments are invalid");

  DurableId operationId = {};
  bool active = false;
  {
    ScopedLock lock(&harness->StateLock);
    if (harness->Faulted)
      return SessionResult_Error(SessionStatus::Faulted,
                                 SVL("agent harness is faulted"));
    active = harness->Running || harness->Suspended;
    if (active) operationId = harness->ActiveOperationId;
  }

  Arena scratch = ArenaCreate();
  Message message = Message_User(&scratch, StrView_Dup(&scratch, text));
  DurableId itemId = DurableId_Generate();
  SessionRecord records[2] = {};
  records[0].EntryId = DurableId_Generate();
  records[0].Kind = SessionRecordKind::QueueItemAccepted;
  records[0].QueueItemAccepted.OperationId = operationId;
  records[0].QueueItemAccepted.ItemId = itemId;
  records[0].QueueItemAccepted.Queue = queue;
  records[0].QueueItemAccepted.Value = message;
  u32 count = 1;
  if (active && queue != QueueKind::NextTurn) {
    records[1].EntryId = DurableId_Generate();
    records[1].Kind = SessionRecordKind::DeferredWrite;
    records[1].DeferredWrite.OperationId = operationId;
    records[1].DeferredWrite.DeferredId = DurableId_Generate();
    records[1].DeferredWrite.QueueItemId = itemId;
    records[1].DeferredWrite.ProvisionedEntryId = DurableId_Generate();
    records[1].DeferredWrite.Value = message;
    count = 2;
  }
  SessionResult appended = AgentHarness_Append(harness, records, count);

  // If the operation ended between the state read and append validation, retain
  // the input as an idle queue item for the next run.
  if (appended.Status == SessionStatus::InvalidArgument && active) {
    records[0].QueueItemAccepted.OperationId = DurableId{};
    count = 1;
    appended = AgentHarness_Append(harness, records, count);
  }
  ArenaRelease(&scratch);
  return appended;
}

SessionResult CloneHarnessEvent(Arena* arena,
                                const AgentHarnessEvent* source,
                                AgentHarnessEvent* out) {
  AgentHarnessEvent clone = *source;
  if (source->Kind == AgentHarnessEventKind::DurableRecord) {
    SessionResult copied =
        SessionRecord_Clone(arena, &source->Record, &clone.Record);
    if (!SessionResult_IsOk(copied)) return copied;
  } else {
    clone.Stream.Delta = StrView_Dup(arena, source->Stream.Delta);
  }
  *out = clone;
  return SessionResult_Ok(source->Cursor);
}

void MarkWatcherLagged(AgentHarnessWatch* watch) {
  watch->Lagged = true;
  watch->Count = 0;
  watch->Head = 0;
  Cond_Broadcast(&watch->Changed);
}

void EnqueueWatcherEvent(AgentHarnessWatch* watch,
                         const AgentHarnessEvent* event) {
  ScopedLock lock(&watch->Lock);
  if (watch->Closed || watch->Lagged) return;
  if (watch->Count == watch->Capacity) {
    MarkWatcherLagged(watch);
    return;
  }
  u32 index = (watch->Head + watch->Count) % watch->Capacity;
  AgentHarnessWatchSlot* slot = &watch->Slots[index];
  if (!slot->Mem.Base)
    slot->Mem = ArenaCreate((usize)16 << 20);
  else
    ArenaReset(&slot->Mem);
  SessionResult copied =
      CloneHarnessEvent(&slot->Mem, event, &slot->Event);
  if (!SessionResult_IsOk(copied)) {
    MarkWatcherLagged(watch);
    return;
  }
  watch->Count++;
  if (event->Kind == AgentHarnessEventKind::DurableRecord)
    watch->Cursor = event->Cursor;
  Cond_Signal(&watch->Changed);
}

SessionResult LoadFullSnapshotLocked(Arena* arena, AgentHarness* harness,
                                     AgentHarnessSnapshot* out) {
  Array<SessionRecord> records = {};
  SessionReduction reduction = {};
  SessionResult loaded =
      AgentHarness_Load(arena, harness, &records, &reduction);
  if (!SessionResult_IsOk(loaded)) return loaded;
  Array<Message> transcript = {};
  SessionResult context = SessionReducer_Context(
      arena, &reduction, reduction.HeadId, &transcript);
  if (!SessionResult_IsOk(context)) return context;
  AgentHarnessSnapshot snapshot = {};
  snapshot.State = reduction;
  snapshot.Transcript = transcript;
  snapshot.Cursor = records.Data[records.Count - 1].Seq;
  snapshot.Running = harness->Running;
  snapshot.Suspended = harness->Suspended;
  snapshot.Faulted = harness->Faulted;
  *out = snapshot;
  return SessionResult_Ok(snapshot.Cursor);
}

void CloseRegisteredWatchers(AgentHarness* harness) {
  for (u32 i = 0; i < harness->WatcherCount; i++) {
    AgentHarnessWatch* watch = harness->Watchers[i];
    ScopedLock lock(&watch->Lock);
    watch->Owner = nullptr;
    watch->Registered = false;
    watch->Closed = true;
    Cond_Broadcast(&watch->Changed);
  }
  harness->WatcherCount = 0;
}

void RefreshAfterOperationCall(AgentHarness* harness,
                               SessionResult operationResult) {
  if (operationResult.Status == SessionStatus::Faulted ||
      operationResult.Status == SessionStatus::IoError) {
    ScopedLock lock(&harness->StateLock);
    harness->Running = false;
    harness->Faulted = true;
    harness->Suspended = true;
    return;
  }

  Arena scratch = ArenaCreate();
  Array<SessionRecord> records = {};
  SessionReduction reduction = {};
  SessionResult loaded =
      AgentHarness_Load(&scratch, harness, &records, &reduction);
  {
    ScopedLock lock(&harness->StateLock);
    harness->Running = false;
    if (!SessionResult_IsOk(loaded)) {
      harness->Faulted = true;
      harness->Suspended = true;
    } else {
      harness->Suspended = reduction.HasActiveOperation;
      harness->ActiveOperationId =
          reduction.HasActiveOperation ? reduction.ActiveOperationId
                                       : DurableId{};
    }
  }
  ArenaRelease(&scratch);
}

}  // namespace

SessionResult AgentHarness_Append(AgentHarness* harness, SessionRecord* records,
                                  u32 count) {
  ScopedLock lock(&harness->StateLock);
  SessionResult result =
      count == 1 ? SessionLog_Append(&harness->Log, records)
                 : SessionLog_AppendBatch(&harness->Log, records, count);
  if (result.Status == SessionStatus::IoError ||
      result.Status == SessionStatus::Faulted) {
    harness->Faulted = true;
  }
  if (SessionResult_IsOk(result)) {
    for (u32 r = 0; r < count; r++) {
      AgentHarnessEvent event = {};
      event.Kind = AgentHarnessEventKind::DurableRecord;
      event.Cursor = records[r].Seq;
      event.Record = records[r];
      for (u32 i = 0; i < harness->WatcherCount; i++) {
        AgentHarnessWatch* watch = harness->Watchers[i];
        if (records[r].Seq > watch->Cursor)
          EnqueueWatcherEvent(watch, &event);
      }
    }
  }
  return result;
}

void AgentHarness_PublishTransient(AgentHarness* harness,
                                   DurableId operationId,
                                   DurableId attemptId,
                                   const StreamEvent* stream) {
  if (!harness || !harness->Initialized || !stream) return;
  ScopedLock lock(&harness->StateLock);
  AgentHarnessEvent event = {};
  event.Kind = AgentHarnessEventKind::TransientStream;
  event.OperationId = operationId;
  event.AttemptId = attemptId;
  event.Stream = *stream;
  for (u32 i = 0; i < harness->WatcherCount; i++) {
    event.Cursor = harness->Watchers[i]->Cursor;
    EnqueueWatcherEvent(harness->Watchers[i], &event);
  }
}

SessionResult AgentHarness_Load(Arena* arena, AgentHarness* harness,
                                Array<SessionRecord>* records,
                                SessionReduction* reduction) {
  SessionResult read = SessionLog_ReadAll(&harness->Log, arena, records);
  if (!SessionResult_IsOk(read)) return read;
  return SessionReducer_Reduce(arena, records->Data, records->Count, reduction);
}

SessionResult AgentHarness_Init(AgentHarness* harness, SessionLog log,
                                const AgentHarnessConfig* config) {
  if (!harness || !config || !log.ReadAll || !log.Append || !log.Writable)
    return Invalid("harness initialization arguments are invalid");
  *harness = AgentHarness{};
  harness->Log = log;
  harness->Config = *config;
  harness->Work = ArenaCreate();
  Mutex_Init(&harness->StateLock);
  harness->Initialized = true;

  Arena scratch = ArenaCreate();
  Array<SessionRecord> records = {};
  SessionReduction reduction = {};
  SessionResult restored =
      AgentHarness_Load(&scratch, harness, &records, &reduction);
  if (!SessionResult_IsOk(restored)) {
    ArenaRelease(&scratch);
    AgentHarness_Close(harness);
    return restored;
  }
  if (reduction.HasActiveOperation) {
    harness->Suspended = true;
    harness->ActiveOperationId = reduction.ActiveOperationId;
    ArenaRelease(&scratch);
    return SessionResult_Error(SessionStatus::Suspended,
                               SVL("session has a suspended operation"),
                               restored.Seq);
  }
  ArenaRelease(&scratch);
  return restored;
}

SessionResult AgentHarness_Close(AgentHarness* harness) {
  if (!harness || !harness->Initialized)
    return SessionResult_Error(SessionStatus::InvalidState,
                               SVL("agent harness is not initialized"));
  {
    ScopedLock lock(&harness->StateLock);
    if (harness->Running)
      return SessionResult_Error(SessionStatus::Busy,
                                 SVL("agent harness is running"));
    CloseRegisteredWatchers(harness);
  }
  SessionLog_Close(&harness->Log);
  Mutex_Destroy(&harness->StateLock);
  ArenaRelease(&harness->Work);
  *harness = AgentHarness{};
  return SessionResult_Ok();
}

SessionResult AgentHarness_Snapshot(Arena* arena, AgentHarness* harness,
                                    SessionReduction* out) {
  if (!arena || !harness || !harness->Initialized || !out)
    return Invalid("snapshot arguments are invalid");
  ScopedLock lock(&harness->StateLock);
  Array<SessionRecord> records = {};
  return AgentHarness_Load(arena, harness, &records, out);
}

SessionResult AgentHarness_SnapshotFull(Arena* arena, AgentHarness* harness,
                                        AgentHarnessSnapshot* out) {
  if (!arena || !harness || !harness->Initialized || !out)
    return Invalid("snapshot arguments are invalid");
  ScopedLock lock(&harness->StateLock);
  return LoadFullSnapshotLocked(arena, harness, out);
}

SessionResult AgentHarness_Watch(Arena* arena, AgentHarness* harness,
                                 u32 capacity,
                                 AgentHarnessSnapshot* outSnapshot,
                                 AgentHarnessWatch* outWatch) {
  if (!arena || !harness || !harness->Initialized || !outSnapshot ||
      !outWatch || outWatch->Initialized)
    return Invalid("watch arguments are invalid");
  if (capacity == 0) capacity = 16;
  if (capacity > AGENT_HARNESS_MAX_WATCH_EVENTS)
    return Invalid("watch capacity exceeds the supported maximum");

  *outWatch = AgentHarnessWatch{};
  outWatch->Owner = harness;
  outWatch->Id = DurableId_Generate();
  outWatch->Capacity = capacity;
  Mutex_Init(&outWatch->Lock);
  Cond_Init(&outWatch->Changed);
  outWatch->Initialized = true;

  SessionResult result = {};
  {
    ScopedLock lock(&harness->StateLock);
    if (harness->WatcherCount == AGENT_HARNESS_MAX_WATCHERS) {
      result = SessionResult_Error(SessionStatus::Busy,
                                   SVL("watcher limit reached"));
    } else {
      result = LoadFullSnapshotLocked(arena, harness, outSnapshot);
      if (SessionResult_IsOk(result)) {
        outWatch->Cursor = outSnapshot->Cursor;
        outWatch->Registered = true;
        harness->Watchers[harness->WatcherCount++] = outWatch;
      }
    }
  }
  if (!SessionResult_IsOk(result)) {
    Cond_Destroy(&outWatch->Changed);
    Mutex_Destroy(&outWatch->Lock);
    *outWatch = AgentHarnessWatch{};
  }
  return result;
}

SessionResult AgentHarness_WatchNext(Arena* arena, AgentHarnessWatch* watch,
                                     AgentHarnessEvent* out) {
  if (!arena || !watch || !watch->Initialized || !out)
    return Invalid("watch next arguments are invalid");
  Mutex_Lock(&watch->Lock);
  watch->Waiting++;
  while (watch->Count == 0 && !watch->Lagged && !watch->Closed)
    Cond_Wait(&watch->Changed, &watch->Lock);
  if (watch->Lagged) {
    watch->Waiting--;
    Mutex_Unlock(&watch->Lock);
    return SessionResult_Error(
        SessionStatus::WatcherLagged,
        SVL("watcher fell behind; take a new snapshot"));
  }
  if (watch->Closed) {
    watch->Waiting--;
    Mutex_Unlock(&watch->Lock);
    return SessionResult_Error(SessionStatus::InvalidState,
                               SVL("watcher is closed"));
  }
  AgentHarnessWatchSlot* slot = &watch->Slots[watch->Head];
  SessionResult copied = CloneHarnessEvent(arena, &slot->Event, out);
  if (SessionResult_IsOk(copied)) {
    ArenaReset(&slot->Mem);
    watch->Head = (watch->Head + 1) % watch->Capacity;
    watch->Count--;
  }
  watch->Waiting--;
  Mutex_Unlock(&watch->Lock);
  return copied;
}

SessionResult AgentHarness_WatchRequestClose(AgentHarnessWatch* watch) {
  if (!watch || !watch->Initialized)
    return Invalid("watch close arguments are invalid");
  AgentHarness* owner = watch->Owner;
  if (owner) {
    ScopedLock ownerLock(&owner->StateLock);
    for (u32 i = 0; i < owner->WatcherCount; i++) {
      if (owner->Watchers[i] != watch) continue;
      for (u32 j = i + 1; j < owner->WatcherCount; j++)
        owner->Watchers[j - 1] = owner->Watchers[j];
      owner->WatcherCount--;
      break;
    }
    ScopedLock watchLock(&watch->Lock);
    watch->Owner = nullptr;
    watch->Registered = false;
    watch->Closed = true;
    Cond_Broadcast(&watch->Changed);
  } else {
    ScopedLock watchLock(&watch->Lock);
    watch->Closed = true;
    Cond_Broadcast(&watch->Changed);
  }
  return SessionResult_Ok();
}

SessionResult AgentHarness_WatchClose(AgentHarnessWatch* watch) {
  if (!watch || !watch->Initialized)
    return Invalid("watch close arguments are invalid");
  AgentHarness* owner = watch->Owner;
  if (owner) {
    ScopedLock ownerLock(&owner->StateLock);
    for (u32 i = 0; i < owner->WatcherCount; i++) {
      if (owner->Watchers[i] != watch) continue;
      for (u32 j = i + 1; j < owner->WatcherCount; j++)
        owner->Watchers[j - 1] = owner->Watchers[j];
      owner->WatcherCount--;
      break;
    }
    ScopedLock watchLock(&watch->Lock);
    watch->Owner = nullptr;
    watch->Registered = false;
    watch->Closed = true;
    Cond_Broadcast(&watch->Changed);
  } else {
    ScopedLock watchLock(&watch->Lock);
    watch->Closed = true;
    Cond_Broadcast(&watch->Changed);
  }
  {
    ScopedLock watchLock(&watch->Lock);
    if (watch->Waiting)
      return SessionResult_Error(
          SessionStatus::Busy,
          SVL("watcher still has a waiting consumer"));
  }
  for (u32 i = 0; i < AGENT_HARNESS_MAX_WATCH_EVENTS; i++)
    ArenaRelease(&watch->Slots[i].Mem);
  Cond_Destroy(&watch->Changed);
  Mutex_Destroy(&watch->Lock);
  *watch = AgentHarnessWatch{};
  return SessionResult_Ok();
}

SessionResult AgentHarness_Run(AgentHarness* harness, StrView prompt) {
  if (!harness || !harness->Initialized || prompt.Length == 0)
    return Invalid("run arguments are invalid");
  {
    ScopedLock lock(&harness->StateLock);
    if (harness->Faulted)
      return SessionResult_Error(SessionStatus::Faulted,
                                 SVL("agent harness is faulted"));
    if (harness->Running)
      return SessionResult_Error(SessionStatus::Busy,
                                 SVL("agent harness is already running"));
    if (harness->Suspended)
      return SessionResult_Error(SessionStatus::Suspended,
                                 SVL("suspended operation must be resolved first"));
    harness->Running = true;
    harness->AbortFlag = 0;
    harness->ActiveOperationId = DurableId_Generate();
  }

  Arena scratch = ArenaCreate();
  Array<SessionRecord> records = {};
  SessionReduction reduction = {};
  SessionResult loaded =
      AgentHarness_Load(&scratch, harness, &records, &reduction);
  DurableId operationId = harness->ActiveOperationId;
  DurableId headId = reduction.HeadId;
  ArenaRelease(&scratch);
  Message accepted =
      Message_User(&harness->Work, StrView_Dup(&harness->Work, prompt));
  SessionResult result = loaded;
  if (SessionResult_IsOk(result) && harness->Config.Hooks.BeforeRun) {
    result = HookFailure(
        harness->Config.Hooks.BeforeRun(harness->Config.Hooks.User,
                                        &harness->Work, &accepted),
        "before-run hook failed");
  }
  if (SessionResult_IsOk(result))
    result =
        AgentDriver_StartRun(harness, &accepted, operationId, headId);

  RefreshAfterOperationCall(harness, result);
  ArenaReset(&harness->Work);
  return result;
}

SessionResult AgentHarness_Compact(AgentHarness* harness) {
  if (!harness || !harness->Initialized)
    return Invalid("compaction arguments are invalid");
  {
    ScopedLock lock(&harness->StateLock);
    if (harness->Faulted)
      return SessionResult_Error(SessionStatus::Faulted,
                                 SVL("agent harness is faulted"));
    if (harness->Running)
      return SessionResult_Error(SessionStatus::Busy,
                                 SVL("agent harness is already running"));
    if (harness->Suspended)
      return SessionResult_Error(SessionStatus::Suspended,
                                 SVL("suspended operation must be resolved first"));
    harness->Running = true;
    harness->AbortFlag = 0;
    harness->ActiveOperationId = DurableId_Generate();
  }

  Arena scratch = ArenaCreate();
  Array<SessionRecord> records = {};
  SessionReduction reduction = {};
  SessionResult loaded =
      AgentHarness_Load(&scratch, harness, &records, &reduction);
  DurableId operationId = harness->ActiveOperationId;
  DurableId headId = reduction.HeadId;
  ArenaRelease(&scratch);
  SessionResult result = loaded;
  if (SessionResult_IsOk(result) && harness->Config.Hooks.BeforeCompaction) {
    result = HookFailure(
        harness->Config.Hooks.BeforeCompaction(
            harness->Config.Hooks.User, &harness->Work, headId),
        "before-compaction hook failed");
  }
  if (SessionResult_IsOk(result))
    result = AgentDriver_Compact(harness, operationId, headId,
                                 DurableId_Generate());
  RefreshAfterOperationCall(harness, result);
  ArenaReset(&harness->Work);
  return result;
}

SessionResult AgentHarness_Navigate(AgentHarness* harness,
                                    DurableId targetEntryId) {
  if (!harness || !harness->Initialized ||
      !DurableId_IsSet(targetEntryId))
    return Invalid("navigation arguments are invalid");
  {
    ScopedLock lock(&harness->StateLock);
    if (harness->Faulted)
      return SessionResult_Error(SessionStatus::Faulted,
                                 SVL("agent harness is faulted"));
    if (harness->Running)
      return SessionResult_Error(SessionStatus::Busy,
                                 SVL("agent harness is already running"));
    if (harness->Suspended)
      return SessionResult_Error(SessionStatus::Suspended,
                                 SVL("suspended operation must be resolved first"));
    harness->Running = true;
    harness->ActiveOperationId = DurableId_Generate();
  }

  Arena scratch = ArenaCreate();
  Array<SessionRecord> records = {};
  SessionReduction reduction = {};
  SessionResult loaded =
      AgentHarness_Load(&scratch, harness, &records, &reduction);
  SessionResult result = loaded;
  if (SessionResult_IsOk(result) &&
      harness->Config.Hooks.BeforeNavigation) {
    result = HookFailure(
        harness->Config.Hooks.BeforeNavigation(
            harness->Config.Hooks.User, &harness->Work, &targetEntryId),
        "before-navigation hook failed");
  }
  bool found = false;
  for (u32 i = 0; SessionResult_IsOk(result) && i < reduction.Nodes.Count; i++)
    if (DurableId_Eq(reduction.Nodes.Data[i].EntryId, targetEntryId)) found = true;
  if (SessionResult_IsOk(result) && !found) {
    result = SessionResult_Error(SessionStatus::NotFound,
                                 SVL("navigation target was not found"));
  } else if (SessionResult_IsOk(result)) {
    SessionRecord started = {};
    started.EntryId = DurableId_Generate();
    started.Kind = SessionRecordKind::OperationStarted;
    started.OperationStarted.OperationId = harness->ActiveOperationId;
    started.OperationStarted.Operation = OperationKind::Navigation;
    started.OperationStarted.StartingHeadId = reduction.HeadId;
    started.OperationStarted.TargetEntryId = targetEntryId;
    result = AgentHarness_Append(harness, &started, 1);
    if (SessionResult_IsOk(result)) {
      OperationOutcome outcome = OperationOutcome::Succeeded;
      StrView error = {};
      SessionResult completionHook = SessionResult_Ok();
      if (harness->Config.Hooks.BeforeOperationComplete) {
        completionHook = HookFailure(
            harness->Config.Hooks.BeforeOperationComplete(
                harness->Config.Hooks.User, &harness->Work,
                OperationKind::Navigation, outcome, error),
            "operation-completion hook failed");
        if (!SessionResult_IsOk(completionHook)) {
          outcome = OperationOutcome::Failed;
          error = completionHook.Message;
        }
      }
      SessionRecord finished = {};
      finished.EntryId = DurableId_Generate();
      finished.Kind = SessionRecordKind::OperationFinished;
      finished.OperationFinished.OperationId = harness->ActiveOperationId;
      finished.OperationFinished.Outcome = outcome;
      finished.OperationFinished.ResultHeadId =
          outcome == OperationOutcome::Succeeded ? targetEntryId
                                                 : reduction.HeadId;
      finished.OperationFinished.Error = error;
      result = AgentHarness_Append(harness, &finished, 1);
      if (SessionResult_IsOk(result) &&
          !SessionResult_IsOk(completionHook))
        result = completionHook;
    }
  }
  ArenaRelease(&scratch);

  RefreshAfterOperationCall(harness, result);
  ArenaReset(&harness->Work);
  return result;
}

SessionResult AgentHarness_LabelBranch(AgentHarness* harness, StrView label) {
  if (!harness || !harness->Initialized || label.Length == 0)
    return Invalid("branch label arguments are invalid");
  {
    ScopedLock lock(&harness->StateLock);
    if (harness->Faulted)
      return SessionResult_Error(SessionStatus::Faulted,
                                 SVL("agent harness is faulted"));
    if (harness->Running)
      return SessionResult_Error(SessionStatus::Busy,
                                 SVL("agent harness is running"));
    if (harness->Suspended)
      return SessionResult_Error(SessionStatus::Suspended,
                                 SVL("suspended operation must be resolved first"));
  }
  Arena scratch = ArenaCreate();
  Array<SessionRecord> records = {};
  SessionReduction reduction = {};
  SessionResult loaded =
      AgentHarness_Load(&scratch, harness, &records, &reduction);
  if (!SessionResult_IsOk(loaded)) {
    ArenaRelease(&scratch);
    return loaded;
  }
  SessionRecord branchLabel = {};
  branchLabel.EntryId = DurableId_Generate();
  branchLabel.Kind = SessionRecordKind::BranchLabel;
  branchLabel.BranchLabelEntry.ParentId = reduction.HeadId;
  branchLabel.BranchLabelEntry.Label = label;
  SessionResult result =
      AgentHarness_Append(harness, &branchLabel, 1);
  ArenaRelease(&scratch);
  return result;
}

SessionResult AgentHarness_SetModel(AgentHarness* harness,
                                    Model model,
                                    Provider provider) {
  if (!harness || !harness->Initialized || model.Id.Length == 0 ||
      model.Provider.Length == 0 || !provider.Stream)
    return Invalid("model arguments are invalid");
  {
    ScopedLock lock(&harness->StateLock);
    if (harness->Faulted)
      return SessionResult_Error(SessionStatus::Faulted,
                                 SVL("agent harness is faulted"));
    if (harness->Running)
      return SessionResult_Error(SessionStatus::Busy,
                                 SVL("agent harness is running"));
    if (harness->Suspended)
      return SessionResult_Error(SessionStatus::Suspended,
                                 SVL("suspended operation must be resolved first"));
    harness->Running = true;
  }
  Arena scratch = ArenaCreate();
  Array<SessionRecord> records = {};
  SessionReduction reduction = {};
  SessionResult loaded =
      AgentHarness_Load(&scratch, harness, &records, &reduction);
  if (!SessionResult_IsOk(loaded)) {
    {
      ScopedLock lock(&harness->StateLock);
      harness->Running = false;
    }
    ArenaRelease(&scratch);
    return loaded;
  }
  SessionRecord setting = {};
  setting.EntryId = DurableId_Generate();
  setting.Kind = SessionRecordKind::Model;
  setting.ModelEntry.ParentId = reduction.HeadId;
  setting.ModelEntry.Provider = model.Provider;
  setting.ModelEntry.Model = model.Id;
  SessionResult result = AgentHarness_Append(harness, &setting, 1);
  {
    ScopedLock lock(&harness->StateLock);
    if (SessionResult_IsOk(result)) {
      harness->Config.ModelInfo = model;
      harness->Config.ProviderImpl = provider;
      harness->Config.Stream.ApiKey = {};
    }
    harness->Running = false;
  }
  ArenaRelease(&scratch);
  return result;
}

SessionResult AgentHarness_Fork(Arena* arena, AgentHarness* harness,
                                SessionRepository* repository,
                                DurableId sourceHeadId,
                                DurableId requestedSessionId, StrView label,
                                SessionLog* outLog, SessionInfo* outInfo) {
  if (!arena || !harness || !harness->Initialized || !repository ||
      !repository->Fork || !outLog || !outInfo)
    return Invalid("fork arguments are invalid");
  {
    ScopedLock lock(&harness->StateLock);
    if (harness->Running)
      return SessionResult_Error(SessionStatus::Busy,
                                 SVL("agent harness is running"));
    if (harness->Suspended)
      return SessionResult_Error(SessionStatus::Suspended,
                                 SVL("suspended operation must be resolved first"));
  }

  Array<SessionRecord> records = {};
  SessionReduction reduction = {};
  SessionResult loaded =
      AgentHarness_Load(arena, harness, &records, &reduction);
  if (!SessionResult_IsOk(loaded)) return loaded;
  const SessionRecord* root = &records.Data[0];
  SessionForkOptions options = {};
  options.SourceSessionId = root->SessionId;
  options.SourceHeadId =
      DurableId_IsSet(sourceHeadId) ? sourceHeadId : reduction.HeadId;
  options.RequestedTargetId = requestedSessionId;
  options.ProjectKey = root->SessionStarted.ProjectKey;
  options.WorkingDirectory = root->SessionStarted.WorkingDirectory;
  options.Label = label;
  return repository->Fork(repository->Ctx, arena, &options, outLog, outInfo);
}

SessionResult AgentHarness_RequestCancel(AgentHarness* harness) {
  if (!harness || !harness->Initialized)
    return Invalid("cancel arguments are invalid");
  DurableId operationId = {};
  {
    ScopedLock lock(&harness->StateLock);
    if (harness->Faulted)
      return SessionResult_Error(SessionStatus::Faulted,
                                 SVL("agent harness is faulted"));
    if (!harness->Running)
      return SessionResult_Error(SessionStatus::InvalidState,
                                 SVL("no operation is running"));
    operationId = harness->ActiveOperationId;
  }
  SessionRecord record = {};
  record.EntryId = DurableId_Generate();
  record.Kind = SessionRecordKind::OperationCancelRequested;
  record.OperationCancelRequested.OperationId = operationId;
  SessionResult appended = AgentHarness_Append(harness, &record, 1);
  if (appended.Status == SessionStatus::InvalidArgument) {
    Arena scratch = ArenaCreate();
    Array<SessionRecord> records = {};
    SessionReduction reduction = {};
    SessionResult loaded =
        AgentHarness_Load(&scratch, harness, &records, &reduction);
    if (SessionResult_IsOk(loaded) && !reduction.HasActiveOperation)
      appended = SessionResult_Error(
          SessionStatus::InvalidState,
          SVL("operation finished before cancellation was recorded"),
          loaded.Seq);
    ArenaRelease(&scratch);
  }
  if (SessionResult_IsOk(appended)) harness->AbortFlag = 1;
  return appended;
}

SessionResult AgentHarness_QueueSteering(AgentHarness* harness, StrView text) {
  return QueueMessage(harness, QueueKind::Steering, text);
}

SessionResult AgentHarness_QueueFollowUp(AgentHarness* harness, StrView text) {
  return QueueMessage(harness, QueueKind::FollowUp, text);
}

SessionResult AgentHarness_QueueNextTurn(AgentHarness* harness, StrView text) {
  return QueueMessage(harness, QueueKind::NextTurn, text);
}

SessionResult AgentHarness_AbortOperation(AgentHarness* harness, StrView reason) {
  if (!harness || !harness->Initialized)
    return Invalid("abort arguments are invalid");
  DurableId operationId = {};
  {
    ScopedLock lock(&harness->StateLock);
    if (!harness->Suspended)
      return SessionResult_Error(SessionStatus::InvalidState,
                                 SVL("no operation is suspended"));
    operationId = harness->ActiveOperationId;
  }
  SessionRecord record = {};
  record.EntryId = DurableId_Generate();
  record.Kind = SessionRecordKind::OperationAborted;
  record.OperationAborted.OperationId = operationId;
  record.OperationAborted.Reason =
      reason.Length ? reason : SVL("aborted during recovery");
  SessionResult appended = AgentHarness_Append(harness, &record, 1);
  if (SessionResult_IsOk(appended)) {
    ScopedLock lock(&harness->StateLock);
    harness->Suspended = false;
    harness->ActiveOperationId = DurableId{};
  }
  return appended;
}

SessionResult AgentHarness_ResumeOperation(AgentHarness* harness) {
  if (!harness || !harness->Initialized)
    return Invalid("resume arguments are invalid");
  {
    ScopedLock lock(&harness->StateLock);
    if (harness->Faulted)
      return SessionResult_Error(SessionStatus::Faulted,
                                 SVL("agent harness is faulted"));
    if (!harness->Suspended)
      return SessionResult_Error(SessionStatus::InvalidState,
                                 SVL("no operation is suspended"));
    if (harness->Running)
      return SessionResult_Error(SessionStatus::Busy,
                                 SVL("agent harness is already running"));
    harness->Running = true;
    harness->AbortFlag = 0;
  }

  Arena scratch = ArenaCreate();
  Array<SessionRecord> records = {};
  SessionReduction reduction = {};
  SessionResult loaded =
      AgentHarness_Load(&scratch, harness, &records, &reduction);
  SessionResult result = loaded;
  if (SessionResult_IsOk(loaded)) {
    if (reduction.Suspension == SessionSuspensionKind::ManualTool) {
      result = SessionResult_Error(
          SessionStatus::RecoveryRequired,
          SVL("interrupted tool requires recovery reconciliation"));
    } else if (reduction.ActiveOperationKind == OperationKind::Run) {
      result = AgentDriver_ResumeRun(harness, &reduction);
    } else if (reduction.ActiveOperationKind == OperationKind::Compaction) {
      result = AgentDriver_ResumeCompaction(harness, &reduction);
    } else {
      result = AgentDriver_ResumeNavigation(harness, &reduction);
    }
  }
  ArenaRelease(&scratch);

  RefreshAfterOperationCall(harness, result);
  ArenaReset(&harness->Work);
  return result;
}

SessionResult AgentHarness_ReconcileTool(AgentHarness* harness,
                                         DurableId toolExecutionId,
                                         RecoveryResolution resolution,
                                         const Message* suppliedResult) {
  if (!harness || !harness->Initialized ||
      !DurableId_IsSet(toolExecutionId))
    return Invalid("tool reconciliation arguments are invalid");
  if ((resolution == RecoveryResolution::SupplyResult) !=
      (suppliedResult != nullptr))
    return Invalid("supply_result requires exactly one supplied result");
  {
    ScopedLock lock(&harness->StateLock);
    if (harness->Faulted)
      return SessionResult_Error(SessionStatus::Faulted,
                                 SVL("agent harness is faulted"));
    if (!harness->Suspended || harness->Running)
      return SessionResult_Error(SessionStatus::InvalidState,
                                 SVL("tool reconciliation requires a suspended operation"));
  }

  Arena scratch = ArenaCreate();
  Array<SessionRecord> records = {};
  SessionReduction reduction = {};
  SessionResult loaded =
      AgentHarness_Load(&scratch, harness, &records, &reduction);
  if (!SessionResult_IsOk(loaded)) {
    ArenaRelease(&scratch);
    return loaded;
  }
  const SessionRecord* pending = nullptr;
  bool alreadyResolved = false;
  for (u32 i = 0; i < records.Count; i++) {
    if (records.Data[i].Kind == SessionRecordKind::RecoveryResolved &&
        DurableId_Eq(records.Data[i].RecoveryResolved.ToolExecutionId,
                     toolExecutionId)) {
      alreadyResolved = true;
      break;
    }
  }
  for (u32 i = 0; i < reduction.PendingTools.Count; i++) {
    if (DurableId_Eq(reduction.PendingTools.Data[i]->ToolStarted.ToolExecutionId,
                     toolExecutionId)) {
      pending = reduction.PendingTools.Data[i];
      break;
    }
  }
  if (!pending) {
    ArenaRelease(&scratch);
    return SessionResult_Error(SessionStatus::NotFound,
                               SVL("pending tool execution was not found"));
  }
  if (alreadyResolved) {
    ArenaRelease(&scratch);
    return SessionResult_Error(SessionStatus::Conflict,
                               SVL("tool recovery was already resolved"));
  }
  if (pending->ToolStarted.ReplayPolicy == ToolReplayPolicy::ReplaySafe &&
      resolution != RecoveryResolution::RerunAnyway) {
    ArenaRelease(&scratch);
    return SessionResult_Error(
        SessionStatus::InvalidArgument,
        SVL("replay-safe tools resume through rerun, not manual result injection"));
  }

  SessionRecord record = {};
  record.EntryId = DurableId_Generate();
  record.Kind = SessionRecordKind::RecoveryResolved;
  record.RecoveryResolved.OperationId = reduction.ActiveOperationId;
  record.RecoveryResolved.ToolExecutionId = toolExecutionId;
  record.RecoveryResolved.Resolution = resolution;
  record.RecoveryResolved.HasSuppliedResult = suppliedResult != nullptr;
  if (suppliedResult) {
    if (suppliedResult->Kind != Role::ToolResult ||
        !StrView_Eq(suppliedResult->ToolCallId,
                    pending->ToolStarted.ToolCallId) ||
        !StrView_Eq(suppliedResult->ToolName, pending->ToolStarted.ToolName)) {
      ArenaRelease(&scratch);
      return Invalid("supplied result does not match the pending tool call");
    }
    record.RecoveryResolved.SuppliedResult = *suppliedResult;
  }
  SessionResult appended = AgentHarness_Append(harness, &record, 1);
  ArenaRelease(&scratch);
  return appended;
}
