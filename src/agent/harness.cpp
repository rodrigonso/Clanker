#include "agent/harness.h"

#include "agent/driver.h"

namespace {

SessionResult EnsureIdleLocked(const AgentHarness* harness, StrView busyMessage) {
  if (harness->Faulted)
    return SessionResult_Error(SessionStatus::Faulted,
                               SVL("agent harness is faulted"));
  if (harness->Running) return SessionResult_Error(SessionStatus::Busy, busyMessage);
  return SessionResult_Ok();
}

// Clones whichever payload the event's kind actually uses. Dispatching on kind
// (rather than duping every field unconditionally) is required now that the
// payloads share storage: touching an inactive member would read garbage.
SessionResult CloneHarnessEvent(Arena* arena, const AgentHarnessEvent* source,
                                AgentHarnessEvent* out) {
  AgentHarnessEvent clone = *source;
  switch (source->Kind) {
    case AgentHarnessEventKind::DurableRecord: {
      SessionRecord* record = ArenaPushStruct(arena, SessionRecord);
      SessionResult copied = SessionRecord_Clone(arena, source->Record, record);
      if (!SessionResult_IsOk(copied)) return copied;
      clone.Record = record;
      break;
    }
    case AgentHarnessEventKind::TransientStream:
      clone.Stream.Delta = StrView_Dup(arena, source->Stream.Delta);
      break;
    case AgentHarnessEventKind::TransientAgentStart:
    case AgentHarnessEventKind::TransientAgentEnd:
    case AgentHarnessEventKind::TransientTurnStart:
    case AgentHarnessEventKind::TransientTurnEnd:
      clone.Turn.Error = StrView_Dup(arena, source->Turn.Error);
      break;
    case AgentHarnessEventKind::TransientToolStart:
    case AgentHarnessEventKind::TransientToolUpdate:
    case AgentHarnessEventKind::TransientToolEnd: {
      clone.Tool.CallId = StrView_Dup(arena, source->Tool.CallId);
      clone.Tool.Name = StrView_Dup(arena, source->Tool.Name);
      clone.Tool.ArgumentsJson = StrView_Dup(arena, source->Tool.ArgumentsJson);
      clone.Tool.Result.Content =
          Array_Make<Content>(arena, source->Tool.Result.Content.Count
                                         ? source->Tool.Result.Content.Count
                                         : 1);
      for (u32 i = 0; i < source->Tool.Result.Content.Count; i++) {
        Content content = source->Tool.Result.Content.Data[i];
        content.Text = StrView_Dup(arena, content.Text);
        content.Signature = StrView_Dup(arena, content.Signature);
        content.ToolId = StrView_Dup(arena, content.ToolId);
        content.ToolName = StrView_Dup(arena, content.ToolName);
        content.ToolArgsJson = StrView_Dup(arena, content.ToolArgsJson);
        content.ImageData = StrView_Dup(arena, content.ImageData);
        content.ImageMimeType = StrView_Dup(arena, content.ImageMimeType);
        Array_Push(&clone.Tool.Result.Content, content);
      }
      break;
    }
    case AgentHarnessEventKind::TransientRetryStart:
    case AgentHarnessEventKind::TransientRetryEnd:
      clone.Retry.Error = StrView_Dup(arena, source->Retry.Error);
      break;
    case AgentHarnessEventKind::TransientQueueUpdate:
    case AgentHarnessEventKind::TransientCancelRequested:
      break;
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

void EnqueueWatcherEvent(AgentHarnessWatch* watch, const AgentHarnessEvent* event) {
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
  SessionResult copied = CloneHarnessEvent(&slot->Mem, event, &slot->Event);
  if (!SessionResult_IsOk(copied)) {
    MarkWatcherLagged(watch);
    return;
  }
  watch->Count++;
  if (event->Kind == AgentHarnessEventKind::DurableRecord) watch->Cursor = event->Cursor;
  Cond_Signal(&watch->Changed);
}

SessionResult CloneReduction(Arena* arena, const SessionReduction* source,
                             SessionReduction* out) {
  SessionReduction copy = *source;
  SessionRecord* records =
      ArenaPushArray(arena, SessionRecord, source->RecordCount);
  for (u32 i = 0; i < source->RecordCount; i++) {
    SessionResult cloned =
        SessionRecord_Clone(arena, &source->Records[i], &records[i]);
    if (!SessionResult_IsOk(cloned)) return cloned;
  }
  copy.Records = records;

  auto cloneIndex = [&](const DurableIdIndex& sourceIndex,
                        DurableIdIndex* destination) {
    *destination = DurableIdIndex{};
    destination->Mem = arena;
    destination->Capacity = sourceIndex.Capacity;
    destination->Count = sourceIndex.Count;
    destination->Slots = ArenaPushArray(
        arena, DurableIdIndexSlot, sourceIndex.Capacity);
    for (u32 i = 0; i < sourceIndex.Capacity; i++)
      destination->Slots[i] = sourceIndex.Slots[i];
  };
  cloneIndex(source->Index, &copy.Index);

  copy.Provider = StrView_Dup(arena, source->Provider);
  copy.Model = StrView_Dup(arena, source->Model);
  copy.ThinkingLevel = StrView_Dup(arena, source->ThinkingLevel);
  copy.ToolNames =
      Array_Make<StrView>(arena, source->ToolNames.Count
                                     ? source->ToolNames.Count
                                     : 1);
  for (u32 i = 0; i < source->ToolNames.Count; i++)
    Array_Push(&copy.ToolNames,
               StrView_Dup(arena, source->ToolNames.Data[i]));
  *out = copy;
  return SessionResult_Ok(copy.RecordCount);
}

// Shared by AgentHarness_SnapshotFull and AgentHarness_Watch, both of which
// already hold StateLock. The caller owns the cloned durable state; transcript
// projection then visits only the active branch.
SessionResult BuildSnapshotLocked(Arena* arena, AgentHarness* harness,
                                  AgentHarnessSnapshot* out) {
  AgentHarnessSnapshot snapshot = {};
  SessionResult cloned =
      CloneReduction(arena, &harness->Reduction, &snapshot.State);
  if (!SessionResult_IsOk(cloned)) return cloned;
  Array<Message> transcript = {};
  SessionResult context =
      SessionReducer_Context(arena, &snapshot.State, snapshot.State.HeadId,
                             &transcript);
  if (!SessionResult_IsOk(context)) return context;
  snapshot.Transcript = transcript;
  snapshot.Cursor =
      harness->Records.Count ? harness->Records.Data[harness->Records.Count - 1].Seq : 0;
  snapshot.Running = harness->Running;
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

void PublishToWatchersLocked(AgentHarness* harness, AgentHarnessEvent* event) {
  for (u32 i = 0; i < harness->WatcherCount; i++) {
    AgentHarnessWatch* watch = harness->Watchers[i];
    event->Cursor = watch->Cursor;
    EnqueueWatcherEvent(watch, event);
  }
}

// Generic "publish to every registered watcher" used by every Publish* below.
void PublishToWatchers(AgentHarness* harness, AgentHarnessEvent* event) {
  ScopedLock lock(&harness->StateLock);
  PublishToWatchersLocked(harness, event);
}

SessionResult QueueMessage(AgentHarness* harness, Array<Message>* queue, StrView text) {
  if (!harness || !harness->Initialized || !queue || text.Length == 0)
    return SessionResult_Invalid("queue arguments are invalid");
  {
    ScopedLock lock(&harness->StateLock);
    if (harness->Faulted)
      return SessionResult_Error(SessionStatus::Faulted, SVL("agent harness is faulted"));
    Message message = Message_User(&harness->Queued, StrView_Dup(&harness->Queued, text));
    Array_Push(queue, message);
  }
  AgentHarness_PublishQueueUpdate(harness);
  return SessionResult_Ok();
}

void ResetQueuesIfEmptyLocked(AgentHarness* harness) {
  if (harness->Queues.Steering.Count || harness->Queues.FollowUp.Count ||
      harness->Queues.NextTurn.Count) {
    return;
  }
  ArenaReset(&harness->Queued);
  harness->Queues.Steering = Array_Make<Message>(&harness->Queued, 4);
  harness->Queues.FollowUp = Array_Make<Message>(&harness->Queued, 4);
  harness->Queues.NextTurn = Array_Make<Message>(&harness->Queued, 4);
}

void CompleteAgentTurnLocked(AgentHarness* harness, AgentTurnKind turn,
                             const SessionResult& result) {
  AgentTurnOutcome outcome =
      SessionResult_IsOk(result)
          ? AgentTurnOutcome::Succeeded
          : result.Status == SessionStatus::Cancelled
                ? AgentTurnOutcome::Aborted
                : AgentTurnOutcome::Failed;
  AgentHarnessEvent event = {};
  event.Kind = AgentHarnessEventKind::TransientAgentEnd;
  event.Turn.Kind = turn;
  event.Turn.Outcome = outcome;
  event.Turn.Error = result.Message;
  PublishToWatchersLocked(harness, &event);
  ArenaReset(&harness->Work);
  harness->Running = false;
}

void ApplyAcceptedCancellationLocked(AgentHarness* harness,
                                     SessionResult* result) {
  if (harness->AbortFlag && result->Status != SessionStatus::IoError &&
      result->Status != SessionStatus::Faulted) {
    *result = SessionResult_Error(SessionStatus::Cancelled,
                                  SVL("operation cancelled"));
  }
}

}  // namespace

SessionResult AgentHarness_Append(AgentHarness* harness, SessionRecord* records,
                                  u32 count, bool rejectIfCancelled) {
  ScopedLock lock(&harness->StateLock);
  if (harness->Faulted)
    return SessionResult_Error(SessionStatus::Faulted, SVL("agent harness is faulted"));
  if (rejectIfCancelled && harness->AbortFlag)
    return SessionResult_Error(SessionStatus::Cancelled,
                               SVL("operation cancelled before commit"));
  SessionResult result = count == 1 ? SessionLog_Append(&harness->Log, &records[0])
                                    : SessionLog_AppendBatch(&harness->Log, records, count);
  if (result.Status == SessionStatus::IoError || result.Status == SessionStatus::Faulted) {
    harness->Faulted = true;
    return result;
  }
  if (!SessionResult_IsOk(result)) return result;

  // The log has assigned Seq/EntryId/CreatedAt into records[i]. Never trust
  // that its payload (message/content text) is safe to read beyond this call
  // -- backends differ in whether the echoed struct's strings live in the
  // repository's own arena or the caller's. Always take our own O(1),
  // self-contained clone into `harness->State`, sourced from records[i] right
  // now while it is still valid; this never re-clones or re-validates any
  // earlier history.
  for (u32 i = 0; i < count; i++) {
    SessionRecord persistent = {};
    SessionResult cloned = SessionRecord_Clone(&harness->State, &records[i], &persistent);
    if (!SessionResult_IsOk(cloned)) {
      harness->Faulted = true;
      return SessionResult_Error(SessionStatus::Faulted,
                                 SVL("harness state clone failed"));
    }
    Array_Push(&harness->Records, persistent);
    SessionReducer_Fold(&harness->Reduction, harness->Records.Data, harness->Records.Count);
    records[i] = persistent;
  }

  for (u32 i = 0; i < count; i++) {
    AgentHarnessEvent event = {};
    event.Kind = AgentHarnessEventKind::DurableRecord;
    event.Cursor = records[i].Seq;
    event.Record = &records[i];
    for (u32 w = 0; w < harness->WatcherCount; w++) {
      AgentHarnessWatch* watch = harness->Watchers[w];
      if (records[i].Seq > watch->Cursor) EnqueueWatcherEvent(watch, &event);
    }
  }
  return SessionResult_Ok(records[count - 1].Seq);
}

SessionReduction AgentHarness_ReduceSnapshot(AgentHarness* harness) {
  ScopedLock lock(&harness->StateLock);
  return harness->Reduction;
}

SessionResult AgentHarness_FlushQueue(AgentHarness* harness, Array<Message>* queue,
                                     DurableId* ioHead, u32* outFlushedCount,
                                     Array<Message>* outMessages) {
  Arena scratch = ArenaCreate();
  Array<Message> pending = {};
  {
    ScopedLock lock(&harness->StateLock);
    if (queue->Count == 0) {
      if (outFlushedCount) *outFlushedCount = 0;
      ArenaRelease(&scratch);
      return SessionResult_Ok();
    }
    pending = Array_Make<Message>(&scratch, queue->Count);
    for (u32 i = 0; i < queue->Count; i++) Array_Push(&pending, queue->Data[i]);
    queue->Count = 0;
  }
  Array<SessionRecord> records = Array_Make<SessionRecord>(&scratch, pending.Count);
  DurableId head = *ioHead;
  for (u32 i = 0; i < pending.Count; i++) {
    SessionRecord record = {};
    record.EntryId = DurableId_Generate();
    record.Kind = SessionRecordKind::Message;
    record.MessageEntry.ParentId = head;
    record.MessageEntry.Value = pending.Data[i];
    Array_Push(&records, record);
    head = record.EntryId;
  }
  SessionResult appended = AgentHarness_Append(harness, records.Data, records.Count);
  if (!SessionResult_IsOk(appended)) {
    ArenaRelease(&scratch);
    return appended;
  }
  {
    ScopedLock lock(&harness->StateLock);
    ResetQueuesIfEmptyLocked(harness);
  }
  // Append rewrote each record in place with its harness-lifetime clone, so
  // these values outlive the scratch arena released below.
  if (outMessages) {
    for (u32 i = 0; i < records.Count; i++)
      Array_Push(outMessages, records.Data[i].MessageEntry.Value);
  }
  ArenaRelease(&scratch);
  *ioHead = head;
  if (outFlushedCount) *outFlushedCount = pending.Count;
  AgentHarness_PublishQueueUpdate(harness);
  return SessionResult_Ok();
}

void AgentHarness_PublishAgentStart(AgentHarness* harness, AgentTurnKind turn) {
  AgentHarnessEvent event = {};
  event.Kind = AgentHarnessEventKind::TransientAgentStart;
  event.Turn.Kind = turn;
  PublishToWatchers(harness, &event);
}

void AgentHarness_PublishAgentEnd(AgentHarness* harness, AgentTurnKind turn,
                                 AgentTurnOutcome outcome, StrView error) {
  AgentHarnessEvent event = {};
  event.Kind = AgentHarnessEventKind::TransientAgentEnd;
  event.Turn.Kind = turn;
  event.Turn.Outcome = outcome;
  event.Turn.Error = error;
  PublishToWatchers(harness, &event);
}

void AgentHarness_PublishTurnStart(AgentHarness* harness, AgentTurnKind turn) {
  AgentHarnessEvent event = {};
  event.Kind = AgentHarnessEventKind::TransientTurnStart;
  event.Turn.Kind = turn;
  PublishToWatchers(harness, &event);
}

void AgentHarness_PublishTurnEnd(AgentHarness* harness, AgentTurnKind turn,
                                AgentTurnOutcome outcome, StrView error) {
  AgentHarnessEvent event = {};
  event.Kind = AgentHarnessEventKind::TransientTurnEnd;
  event.Turn.Kind = turn;
  event.Turn.Outcome = outcome;
  event.Turn.Error = error;
  PublishToWatchers(harness, &event);
}

void AgentHarness_PublishStream(AgentHarness* harness, const StreamEvent* streamEvent) {
  if (!streamEvent) return;
  AgentHarnessEvent event = {};
  event.Kind = AgentHarnessEventKind::TransientStream;
  event.Stream = *streamEvent;
  PublishToWatchers(harness, &event);
}

void AgentHarness_PublishToolStart(AgentHarness* harness, StrView toolCallId,
                                   StrView toolName, StrView argumentsJson) {
  AgentHarnessEvent event = {};
  event.Kind = AgentHarnessEventKind::TransientToolStart;
  event.Tool.CallId = toolCallId;
  event.Tool.Name = toolName;
  event.Tool.ArgumentsJson = argumentsJson;
  PublishToWatchers(harness, &event);
}

void AgentHarness_PublishToolUpdate(AgentHarness* harness, StrView toolCallId,
                                    StrView toolName, const ToolResult* partial) {
  if (!partial) return;
  AgentHarnessEvent event = {};
  event.Kind = AgentHarnessEventKind::TransientToolUpdate;
  event.Tool.CallId = toolCallId;
  event.Tool.Name = toolName;
  event.Tool.Result = *partial;
  PublishToWatchers(harness, &event);
}

void AgentHarness_PublishToolEnd(AgentHarness* harness, StrView toolCallId,
                                StrView toolName, const ToolResult* result,
                                bool isError) {
  if (!result) return;
  AgentHarnessEvent event = {};
  event.Kind = AgentHarnessEventKind::TransientToolEnd;
  event.Tool.CallId = toolCallId;
  event.Tool.Name = toolName;
  event.Tool.Result = *result;
  event.Tool.IsError = isError;
  PublishToWatchers(harness, &event);
}

void AgentHarness_PublishRetry(AgentHarness* harness, AgentHarnessEventKind kind,
                               RetryLayer layer, u32 attempt, u32 maxAttempts,
                               u32 delayMs, StrView error) {
  AgentHarnessEvent event = {};
  event.Kind = kind;
  event.Retry.Layer = layer;
  event.Retry.Attempt = attempt;
  event.Retry.MaxAttempts = maxAttempts;
  event.Retry.DelayMs = delayMs;
  event.Retry.Error = error;
  PublishToWatchers(harness, &event);
}

void AgentHarness_PublishQueueUpdate(AgentHarness* harness) {
  AgentHarnessEvent event = {};
  event.Kind = AgentHarnessEventKind::TransientQueueUpdate;
  ScopedLock lock(&harness->StateLock);
  event.Queue.SteerCount = harness->Queues.Steering.Count;
  event.Queue.FollowUpCount = harness->Queues.FollowUp.Count;
  event.Queue.NextTurnCount = harness->Queues.NextTurn.Count;
  PublishToWatchersLocked(harness, &event);
}

SessionResult AgentHarness_Init(AgentHarness* harness, SessionLog log,
                                const AgentHarnessConfig* config) {
  if (!harness || !config || !log.ReadAll || !log.Append || !log.Writable ||
      !Model_RequestProfileIsValid(&config->ModelInfo, config->Stream.Profile))
    return SessionResult_Invalid("harness initialization arguments are invalid");
  *harness = AgentHarness{};
  harness->Log = log;
  harness->Config = *config;
  harness->Config.Stream.Profile = Model_ResolveRequestProfile(
      &harness->Config.ModelInfo, harness->Config.Stream.Profile);
  harness->Runtime = ArenaCreate();
  harness->State = ArenaCreate();
  harness->Queued = ArenaCreate();
  harness->Work = ArenaCreate();
  Mutex_Init(&harness->StateLock);
  harness->Initialized = true;
  harness->Queues.Steering = Array_Make<Message>(&harness->Queued, 4);
  harness->Queues.FollowUp = Array_Make<Message>(&harness->Queued, 4);
  harness->Queues.NextTurn = Array_Make<Message>(&harness->Queued, 4);

  if (config->ToolCount) {
    harness->RuntimeTools = ArenaPushArray(&harness->Runtime, ToolRuntime, config->ToolCount);
    for (u32 i = 0; i < config->ToolCount; i++) {
      ToolRuntime* runtime = &harness->RuntimeTools[i];
      *runtime = ToolRuntime{};
      runtime->Definition = &config->Tools[i];
      StrBuf schemaError = StrBuf_Make(&harness->Runtime, 128);
      StrView schema = config->Tools[i].ParamsJsonSchema.Length
                           ? config->Tools[i].ParamsJsonSchema
                           : SVL("{\"type\":\"object\",\"properties\":{}}");
      if (!JsonSchema_Compile(&harness->Runtime, schema, &runtime->Schema, &schemaError)) {
        AgentHarness_Close(harness);
        return SessionResult_Invalid("tool parameter schema is invalid or unsupported");
      }
    }
  }

  // The one and only ReadAll for this harness instance: SessionLog_ReadAll
  // already deep-clones every record into the arena it is given, so passing
  // `harness->State` directly makes the loaded array self-contained with no
  // extra copy. SessionReducer_Reduce then does its one-time O(n) bulk
  // validation + index build; every append from here on is O(1) incremental
  // (SessionReducer_Fold / SessionReducer_ValidateAppend), never a reload.
  Array<SessionRecord> loaded = {};
  SessionResult read = SessionLog_ReadAll(&harness->Log, &harness->State, &loaded);
  if (!SessionResult_IsOk(read)) {
    AgentHarness_Close(harness);
    return read;
  }
  harness->Records = loaded;

  SessionResult reduced = SessionReducer_Reduce(&harness->State, harness->Records.Data,
                                                harness->Records.Count, &harness->Reduction);
  if (!SessionResult_IsOk(reduced)) {
    AgentHarness_Close(harness);
    return reduced;
  }
  return reduced;
}

SessionResult AgentHarness_Close(AgentHarness* harness) {
  if (!harness || !harness->Initialized)
    return SessionResult_Error(SessionStatus::InvalidState,
                               SVL("agent harness is not initialized"));
  {
    ScopedLock lock(&harness->StateLock);
    if (harness->Running)
      return SessionResult_Error(SessionStatus::Busy, SVL("agent harness is running"));
    CloseRegisteredWatchers(harness);
  }
  SessionLog_Close(&harness->Log);
  Mutex_Destroy(&harness->StateLock);
  ArenaRelease(&harness->Runtime);
  ArenaRelease(&harness->State);
  ArenaRelease(&harness->Queued);
  ArenaRelease(&harness->Work);
  *harness = AgentHarness{};
  return SessionResult_Ok();
}

SessionResult AgentHarness_Snapshot(Arena* arena, AgentHarness* harness,
                                    SessionReduction* out) {
  if (!arena || !harness || !harness->Initialized || !out)
    return SessionResult_Invalid("snapshot arguments are invalid");
  ScopedLock lock(&harness->StateLock);
  return CloneReduction(arena, &harness->Reduction, out);
}

SessionResult AgentHarness_SnapshotFull(Arena* arena, AgentHarness* harness,
                                        AgentHarnessSnapshot* out) {
  if (!arena || !harness || !harness->Initialized || !out)
    return SessionResult_Invalid("snapshot arguments are invalid");
  ScopedLock lock(&harness->StateLock);
  return BuildSnapshotLocked(arena, harness, out);
}

SessionResult AgentHarness_Watch(Arena* arena, AgentHarness* harness, u32 capacity,
                                 AgentHarnessSnapshot* outSnapshot,
                                 AgentHarnessWatch* outWatch) {
  if (!arena || !harness || !harness->Initialized || !outSnapshot || !outWatch ||
      outWatch->Initialized)
    return SessionResult_Invalid("watch arguments are invalid");
  if (capacity == 0) capacity = 16;
  if (capacity > AGENT_HARNESS_MAX_WATCH_EVENTS)
    return SessionResult_Invalid("watch capacity exceeds the supported maximum");

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
      result = SessionResult_Error(SessionStatus::Busy, SVL("watcher limit reached"));
    } else {
      result = BuildSnapshotLocked(arena, harness, outSnapshot);
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
    return SessionResult_Invalid("watch next arguments are invalid");
  Mutex_Lock(&watch->Lock);
  watch->Waiting++;
  while (watch->Count == 0 && !watch->Lagged && !watch->Closed)
    Cond_Wait(&watch->Changed, &watch->Lock);
  if (watch->Lagged) {
    watch->Waiting--;
    Mutex_Unlock(&watch->Lock);
    return SessionResult_Error(SessionStatus::WatcherLagged,
                               SVL("watcher fell behind; take a new snapshot"));
  }
  // Closing stops further publication; it does not discard what was already
  // published. Drain the buffer first so a consumer shutting down right after a
  // run still receives that run's trailing turn_end/agent_end.
  if (watch->Count == 0) {
    watch->Waiting--;
    Mutex_Unlock(&watch->Lock);
    return SessionResult_Error(SessionStatus::InvalidState, SVL("watcher is closed"));
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
  if (!watch || !watch->Initialized) return SessionResult_Invalid("watch close arguments are invalid");
  AgentHarness* owner = watch->Owner;
  if (owner) {
    ScopedLock ownerLock(&owner->StateLock);
    for (u32 i = 0; i < owner->WatcherCount; i++) {
      if (owner->Watchers[i] != watch) continue;
      for (u32 j = i + 1; j < owner->WatcherCount; j++) owner->Watchers[j - 1] = owner->Watchers[j];
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
  if (!watch || !watch->Initialized) return SessionResult_Invalid("watch close arguments are invalid");
  AgentHarness* owner = watch->Owner;
  if (owner) {
    ScopedLock ownerLock(&owner->StateLock);
    for (u32 i = 0; i < owner->WatcherCount; i++) {
      if (owner->Watchers[i] != watch) continue;
      for (u32 j = i + 1; j < owner->WatcherCount; j++) owner->Watchers[j - 1] = owner->Watchers[j];
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
      return SessionResult_Error(SessionStatus::Busy,
                                 SVL("watcher still has a waiting consumer"));
  }
  for (u32 i = 0; i < AGENT_HARNESS_MAX_WATCH_EVENTS; i++) ArenaRelease(&watch->Slots[i].Mem);
  Cond_Destroy(&watch->Changed);
  Mutex_Destroy(&watch->Lock);
  *watch = AgentHarnessWatch{};
  return SessionResult_Ok();
}

SessionResult AgentHarness_Run(AgentHarness* harness, StrView prompt) {
  if (!harness || !harness->Initialized || prompt.Length == 0)
    return SessionResult_Invalid("run arguments are invalid");
  {
    ScopedLock lock(&harness->StateLock);
    SessionResult idle = EnsureIdleLocked(harness, SVL("agent harness is already running"));
    if (!SessionResult_IsOk(idle)) return idle;
    harness->Running = true;
    harness->AbortFlag = 0;
  }

  SessionResult result = AgentDriver_Run(harness, prompt);

  {
    ScopedLock lock(&harness->StateLock);
    ApplyAcceptedCancellationLocked(harness, &result);
    if (result.Status == SessionStatus::IoError || result.Status == SessionStatus::Faulted)
      harness->Faulted = true;
    CompleteAgentTurnLocked(harness, AgentTurnKind::Run, result);
  }
  return result;
}

SessionResult AgentHarness_Compact(AgentHarness* harness) {
  if (!harness || !harness->Initialized) return SessionResult_Invalid("compaction arguments are invalid");
  {
    ScopedLock lock(&harness->StateLock);
    SessionResult idle = EnsureIdleLocked(harness, SVL("agent harness is already running"));
    if (!SessionResult_IsOk(idle)) return idle;
    harness->Running = true;
    harness->AbortFlag = 0;
  }

  SessionResult result = AgentDriver_Compact(harness);

  {
    ScopedLock lock(&harness->StateLock);
    ApplyAcceptedCancellationLocked(harness, &result);
    if (result.Status == SessionStatus::IoError || result.Status == SessionStatus::Faulted)
      harness->Faulted = true;
    CompleteAgentTurnLocked(harness, AgentTurnKind::Compaction, result);
  }
  return result;
}

SessionResult AgentHarness_Navigate(AgentHarness* harness, DurableId targetEntryId) {
  if (!harness || !harness->Initialized || !DurableId_IsSet(targetEntryId))
    return SessionResult_Invalid("navigation arguments are invalid");
  {
    ScopedLock lock(&harness->StateLock);
    SessionResult idle = EnsureIdleLocked(harness, SVL("agent harness is already running"));
    if (!SessionResult_IsOk(idle)) return idle;
    harness->Running = true;
    harness->AbortFlag = 0;
  }

  DurableId target = targetEntryId;
  SessionResult result = SessionResult_Ok();
  if (harness->Config.Hooks.BeforeNavigation) {
    result = AgentHook_Failure(harness->Config.Hooks.BeforeNavigation(harness->Config.Hooks.User,
                                                                &harness->Work, &target),
                         "before-navigation hook failed");
  }

  if (SessionResult_IsOk(result)) {
    ScopedLock lock(&harness->StateLock);
    u32 recordIndex = 0;
    if (!DurableIdIndex_Find(&harness->Reduction.Index, target, &recordIndex) ||
        !SessionRecord_IsTreeNode(
            harness->Reduction.Records[recordIndex].Kind))
      result = SessionResult_Error(SessionStatus::NotFound,
                                   SVL("navigation target was not found"));
  }

  if (SessionResult_IsOk(result)) {
    SessionRecord leaf = {};
    leaf.EntryId = DurableId_Generate();
    leaf.Kind = SessionRecordKind::Leaf;
    leaf.LeafEntry.LeafId = target;
    result = AgentHarness_Append(harness, &leaf, 1, true);
  }

  {
    ScopedLock lock(&harness->StateLock);
    ApplyAcceptedCancellationLocked(harness, &result);
    if (result.Status == SessionStatus::IoError || result.Status == SessionStatus::Faulted)
      harness->Faulted = true;
    ArenaReset(&harness->Work);
    harness->Running = false;
  }
  return result;
}

SessionResult AgentHarness_LabelBranch(AgentHarness* harness, StrView label) {
  if (!harness || !harness->Initialized || label.Length == 0)
    return SessionResult_Invalid("branch label arguments are invalid");
  DurableId head;
  {
    ScopedLock lock(&harness->StateLock);
    SessionResult idle = EnsureIdleLocked(harness, SVL("agent harness is running"));
    if (!SessionResult_IsOk(idle)) return idle;
    head = harness->Reduction.HeadId;
  }
  SessionRecord branchLabel = {};
  branchLabel.EntryId = DurableId_Generate();
  branchLabel.Kind = SessionRecordKind::BranchLabel;
  branchLabel.BranchLabelEntry.ParentId = head;
  branchLabel.BranchLabelEntry.Label = label;
  return AgentHarness_Append(harness, &branchLabel, 1);
}

SessionResult AgentHarness_SetName(AgentHarness* harness, StrView name) {
  if (!harness || !harness->Initialized) return SessionResult_Invalid("session name arguments are invalid");
  {
    ScopedLock lock(&harness->StateLock);
    SessionResult idle = EnsureIdleLocked(harness, SVL("agent harness is running"));
    if (!SessionResult_IsOk(idle)) return idle;
  }
  SessionRecord record = {};
  record.EntryId = DurableId_Generate();
  record.Kind = SessionRecordKind::SessionName;
  record.SessionNameEntry.Name = name;
  return AgentHarness_Append(harness, &record, 1);
}

SessionResult AgentHarness_SetModel(AgentHarness* harness, Model model, Provider provider,
                                    ModelRequestProfile profile) {
  if (!harness || !harness->Initialized || model.Id.Length == 0 ||
      model.Provider.Length == 0 || !provider.Stream ||
      !Model_RequestProfileIsValid(&model, profile))
    return SessionResult_Invalid("model arguments are invalid");
  profile = Model_ResolveRequestProfile(&model, profile);
  DurableId head;
  {
    ScopedLock lock(&harness->StateLock);
    SessionResult idle = EnsureIdleLocked(harness, SVL("agent harness is running"));
    if (!SessionResult_IsOk(idle)) return idle;
    harness->Running = true;
    head = harness->Reduction.HeadId;
  }
  SessionRecord setting = {};
  setting.EntryId = DurableId_Generate();
  setting.Kind = SessionRecordKind::Model;
  setting.ModelEntry.ParentId = head;
  setting.ModelEntry.Provider = model.Provider;
  setting.ModelEntry.Model = model.Id;
  SessionResult result = AgentHarness_Append(harness, &setting, 1);
  {
    ScopedLock lock(&harness->StateLock);
    if (SessionResult_IsOk(result)) {
      harness->Config.ModelInfo = model;
      harness->Config.ProviderImpl = provider;
      harness->Config.Stream.Auth = {};
      harness->Config.Stream.Profile = profile;
    }
    harness->Running = false;
  }
  return result;
}

SessionResult AgentHarness_Fork(Arena* arena, AgentHarness* harness,
                                SessionRepository* repository, DurableId sourceHeadId,
                                DurableId requestedSessionId, StrView label,
                                SessionLog* outLog, SessionInfo* outInfo) {
  if (!arena || !harness || !harness->Initialized || !repository || !repository->Fork ||
      !outLog || !outInfo)
    return SessionResult_Invalid("fork arguments are invalid");

  DurableId sessionId;
  DurableId headId;
  StrView projectKey;
  StrView workingDirectory;
  {
    ScopedLock lock(&harness->StateLock);
    if (harness->Running)
      return SessionResult_Error(SessionStatus::Busy, SVL("agent harness is running"));
    if (harness->Records.Count == 0)
      return SessionResult_Error(SessionStatus::InvalidState, SVL("agent harness has no session root"));
    const SessionRecord* root = &harness->Records.Data[0];
    sessionId = root->SessionId;
    headId = DurableId_IsSet(sourceHeadId) ? sourceHeadId : harness->Reduction.HeadId;
    projectKey = root->SessionStarted.ProjectKey;
    workingDirectory = root->SessionStarted.WorkingDirectory;
  }

  SessionForkOptions options = {};
  options.SourceSessionId = sessionId;
  options.SourceHeadId = headId;
  options.RequestedTargetId = requestedSessionId;
  options.ProjectKey = projectKey;
  options.WorkingDirectory = workingDirectory;
  options.Label = label;
  return repository->Fork(repository->Ctx, arena, &options, outLog, outInfo);
}

SessionResult AgentHarness_RequestCancel(AgentHarness* harness) {
  if (!harness || !harness->Initialized) return SessionResult_Invalid("cancel arguments are invalid");
  {
    ScopedLock lock(&harness->StateLock);
    if (harness->Faulted)
      return SessionResult_Error(SessionStatus::Faulted, SVL("agent harness is faulted"));
    if (!harness->Running)
      return SessionResult_Error(SessionStatus::InvalidState, SVL("no operation is running"));
    harness->AbortFlag = 1;
    AgentHarnessEvent event = {};
    event.Kind = AgentHarnessEventKind::TransientCancelRequested;
    PublishToWatchersLocked(harness, &event);
  }
  return SessionResult_Ok();
}

SessionResult AgentHarness_QueueSteering(AgentHarness* harness, StrView text) {
  return QueueMessage(harness, &harness->Queues.Steering, text);
}

SessionResult AgentHarness_QueueFollowUp(AgentHarness* harness, StrView text) {
  return QueueMessage(harness, &harness->Queues.FollowUp, text);
}

SessionResult AgentHarness_QueueNextTurn(AgentHarness* harness, StrView text) {
  return QueueMessage(harness, &harness->Queues.NextTurn, text);
}
