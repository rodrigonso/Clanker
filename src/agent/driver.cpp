#include "agent/driver.h"

#include "json/json.h"

namespace {

SessionRecord NewRecord(SessionRecordKind kind) {
  SessionRecord record = {};
  record.EntryId = DurableId_Generate();
  record.Kind = kind;
  return record;
}

struct HarnessStreamBridge {
  AgentHarness* Harness;
  StreamSink Downstream;
  DurableId OperationId;
  DurableId AttemptId;
};

void HarnessStreamEvent(void* context, const StreamEvent& event) {
  HarnessStreamBridge* bridge = (HarnessStreamBridge*)context;
  AgentHarness_PublishTransient(bridge->Harness, bridge->OperationId,
                                bridge->AttemptId, &event);
  StreamSink_Emit(bridge->Downstream, event);
}

SessionResult Load(AgentHarness* harness, Arena* arena,
                   Array<SessionRecord>* records, SessionReduction* reduction) {
  return AgentHarness_Load(arena, harness, records, reduction);
}

SessionResult HookFailure(SessionResult result, const char* fallback) {
  if (SessionResult_IsOk(result)) return result;
  return SessionResult_Error(
      SessionStatus::HookError,
      result.Message.Length ? result.Message : SV(fallback), result.Seq);
}

StrView EffectiveRequestJson(Arena* arena, const Model* model,
                             const StreamOptions* options,
                             const LlmContext* context) {
  StrBuf json = StrBuf_Make(arena, 256);
  StrBuf_AppendCStr(&json, "{\"provider\":");
  Json_WriteString(&json, model->Provider);
  StrBuf_AppendCStr(&json, ",\"model\":");
  Json_WriteString(&json, model->Id);
  StrBuf_AppendCStr(&json, ",\"systemPrompt\":");
  Json_WriteString(&json, context->SystemPrompt);
  StrBuf_AppendCStr(&json, ",\"messageCount\":");
  StrBuf_AppendU64(&json, context->MessageCount);
  StrBuf_AppendCStr(&json, ",\"toolCount\":");
  StrBuf_AppendU64(&json, context->ToolCount);
  StrBuf_AppendCStr(&json, ",\"maxTokens\":");
  StrBuf_AppendI64(&json, options->MaxTokens);
  StrBuf_AppendChar(&json, '}');
  return StrBuf_View(&json);
}

const SessionRecord* FindDeferred(const Array<SessionRecord>* records,
                                  DurableId itemId) {
  for (u32 i = 0; i < records->Count; i++) {
    const SessionRecord* deferred = &records->Data[i];
    if (deferred->Kind == SessionRecordKind::DeferredWrite &&
        DurableId_Eq(deferred->DeferredWrite.QueueItemId, itemId))
      return deferred;
  }
  return nullptr;
}

const SessionRecord* FindDeferredCommit(const Array<SessionRecord>* records,
                                        DurableId deferredId) {
  for (u32 i = 0; i < records->Count; i++) {
    const SessionRecord* committed = &records->Data[i];
    if (committed->Kind == SessionRecordKind::DeferredWriteCommitted &&
        DurableId_Eq(committed->DeferredWriteCommitted.DeferredId, deferredId))
      return committed;
  }
  return nullptr;
}

const SessionRecord* FindEntry(const Array<SessionRecord>* records,
                               DurableId entryId) {
  for (u32 i = 0; i < records->Count; i++)
    if (DurableId_Eq(records->Data[i].EntryId, entryId))
      return &records->Data[i];
  return nullptr;
}

const SessionRecord* LatestRecovery(const Array<SessionRecord>* records,
                                    DurableId toolExecutionId) {
  const SessionRecord* result = nullptr;
  for (u32 i = 0; i < records->Count; i++) {
    if (records->Data[i].Kind == SessionRecordKind::RecoveryResolved &&
        DurableId_Eq(records->Data[i].RecoveryResolved.ToolExecutionId,
                     toolExecutionId))
      result = &records->Data[i];
  }
  return result;
}

SessionResult CommitQueueCheckpoint(AgentHarness* harness, QueueKind queue,
                                    DurableId operationId, DurableId* ioHead,
                                    u32* outCommitted) {
  Arena scratch = ArenaCreate();
  Array<SessionRecord> records = {};
  SessionReduction reduction = {};
  SessionResult loaded = Load(harness, &scratch, &records, &reduction);
  if (!SessionResult_IsOk(loaded)) {
    ArenaRelease(&scratch);
    return loaded;
  }

  u32 selected = 0;
  for (u32 i = 0; i < reduction.PendingQueueItems.Count; i++)
    if (reduction.PendingQueueItems.Data[i]->QueueItemAccepted.Queue == queue)
      selected++;
  if (selected == 0) {
    *outCommitted = 0;
    ArenaRelease(&scratch);
    return loaded;
  }

  Array<SessionRecord> writes =
      Array_Make<SessionRecord>(&scratch, selected * 2 + 1);
  Array<DurableId> itemIds = Array_Make<DurableId>(&scratch, selected);
  DurableId head = *ioHead;
  for (u32 i = 0; i < reduction.PendingQueueItems.Count; i++) {
    const SessionRecord* item = reduction.PendingQueueItems.Data[i];
    if (item->QueueItemAccepted.Queue != queue) continue;
    const SessionRecord* deferred =
        FindDeferred(&records, item->QueueItemAccepted.ItemId);
    DurableId messageId = deferred ? deferred->DeferredWrite.ProvisionedEntryId
                                   : item->QueueItemAccepted.ItemId;
    const SessionRecord* existing = FindEntry(&records, messageId);
    if (existing && existing->Kind != SessionRecordKind::Message) {
      ArenaRelease(&scratch);
      return SessionResult_Error(
          SessionStatus::Corrupt,
          SVL("queue item provisioned ID is not a message"));
    }
    if (!existing) {
      SessionRecord message = NewRecord(SessionRecordKind::Message);
      message.EntryId = messageId;
      message.MessageEntry.Value =
          deferred ? deferred->DeferredWrite.Value
                   : item->QueueItemAccepted.Value;
      message.MessageEntry.ParentId = head;
      Array_Push(&writes, message);
      head = message.EntryId;
    }

    if (deferred &&
        !FindDeferredCommit(&records, deferred->DeferredWrite.DeferredId)) {
      SessionRecord committed =
          NewRecord(SessionRecordKind::DeferredWriteCommitted);
      committed.DeferredWriteCommitted.OperationId = operationId;
      committed.DeferredWriteCommitted.DeferredId =
          deferred->DeferredWrite.DeferredId;
      committed.DeferredWriteCommitted.MessageEntryId = messageId;
      Array_Push(&writes, committed);
    }
    Array_Push(&itemIds, item->QueueItemAccepted.ItemId);
  }
  SessionRecord checkpoint = NewRecord(SessionRecordKind::QueueCheckpoint);
  checkpoint.QueueCheckpoint.OperationId = operationId;
  checkpoint.QueueCheckpoint.Checkpoint =
      queue == QueueKind::Steering
          ? QueueCheckpointKind::AfterToolBatch
          : (queue == QueueKind::FollowUp ? QueueCheckpointKind::AfterRun
                                          : QueueCheckpointKind::BeforeAssistant);
  checkpoint.QueueCheckpoint.ItemIds = itemIds;
  Array_Push(&writes, checkpoint);

  SessionResult appended =
      AgentHarness_Append(harness, writes.Data, writes.Count);
  if (SessionResult_IsOk(appended)) {
    *ioHead = head;
    *outCommitted = selected;
  }
  ArenaRelease(&scratch);
  return appended;
}

SessionResult FinishOperation(AgentHarness* harness, DurableId operationId,
                              DurableId head, OperationOutcome outcome,
                              StrView error) {
  SessionResult hookResult = SessionResult_Ok();
  if (harness->Config.Hooks.BeforeOperationComplete) {
    Arena scratch = ArenaCreate();
    Array<SessionRecord> records = {};
    SessionReduction reduction = {};
    SessionResult loaded =
        Load(harness, &scratch, &records, &reduction);
    if (!SessionResult_IsOk(loaded)) {
      ArenaRelease(&scratch);
      return loaded;
    }
    hookResult = HookFailure(
        harness->Config.Hooks.BeforeOperationComplete(
            harness->Config.Hooks.User, &harness->Work,
            reduction.ActiveOperationKind, outcome, error),
        "operation-completion hook failed");
    if (!SessionResult_IsOk(hookResult)) {
      outcome = OperationOutcome::Failed;
      error = hookResult.Message;
    }
    ArenaRelease(&scratch);
  }
  SessionRecord finished = NewRecord(SessionRecordKind::OperationFinished);
  finished.OperationFinished.OperationId = operationId;
  finished.OperationFinished.Outcome = outcome;
  finished.OperationFinished.ResultHeadId = head;
  finished.OperationFinished.Error = error;
  SessionResult appended = AgentHarness_Append(harness, &finished, 1);
  return SessionResult_IsOk(appended) && !SessionResult_IsOk(hookResult)
             ? SessionResult_Error(SessionStatus::HookError,
                                   hookResult.Message, appended.Seq)
             : appended;
}

SessionResult RunProvider(AgentHarness* harness, DurableId operationId,
                          DurableId* ioHead, Message* outAssistant,
                          bool* outProviderOk) {
  for (u32 attemptIndex = 0;; attemptIndex++) {
    Arena scratch = ArenaCreate();
    Array<SessionRecord> records = {};
    SessionReduction reduction = {};
    SessionResult loaded = Load(harness, &scratch, &records, &reduction);
    if (!SessionResult_IsOk(loaded)) {
      ArenaRelease(&scratch);
      return loaded;
    }
    Array<Message> contextMessages = {};
    SessionResult contextResult = SessionReducer_Context(
        &scratch, &reduction, *ioHead, &contextMessages);
    if (!SessionResult_IsOk(contextResult)) {
      ArenaRelease(&scratch);
      return contextResult;
    }
    if (harness->Config.Hooks.BeforeContext) {
      SessionResult hooked = HookFailure(
          harness->Config.Hooks.BeforeContext(
              harness->Config.Hooks.User, &scratch, OperationKind::Run,
              *ioHead, &contextMessages),
          "before-context hook failed");
      if (!SessionResult_IsOk(hooked)) {
        ArenaRelease(&scratch);
        return hooked;
      }
    }
    AgentProviderRequest request = {};
    AgentProvider_BuildRequest(
        &scratch, harness->Config.SystemPrompt, contextMessages.Data,
        contextMessages.Count, harness->Config.Tools, harness->Config.ToolCount,
        &request);

    DurableId attemptId = DurableId_Generate();
    DurableId assistantId = DurableId_Generate();
    AgentHarnessProviderInput effective = {};
    effective.ModelInfo = harness->Config.ModelInfo;
    effective.Context = request.Context;
    effective.Options = harness->Config.Stream;
    if (harness->Config.Hooks.BeforeProvider) {
      SessionResult hooked = HookFailure(
          harness->Config.Hooks.BeforeProvider(
              harness->Config.Hooks.User, &scratch, OperationKind::Run,
              attemptIndex, &effective),
          "before-provider hook failed");
      if (!SessionResult_IsOk(hooked)) {
        ArenaRelease(&scratch);
        return hooked;
      }
    }
    if (effective.ModelInfo.Provider.Length == 0 ||
        effective.ModelInfo.Id.Length == 0 ||
        effective.Options.MaxTokens < 0) {
      ArenaRelease(&scratch);
      return SessionResult_Error(
          SessionStatus::HookError,
          SVL("before-provider hook produced invalid settings"));
    }
    SessionRecord started = NewRecord(SessionRecordKind::GenerationStarted);
    started.GenerationStarted.OperationId = operationId;
    started.GenerationStarted.AttemptId = attemptId;
    started.GenerationStarted.Provider = effective.ModelInfo.Provider;
    started.GenerationStarted.Model = effective.ModelInfo.Id;
    started.GenerationStarted.EffectiveRequestJson =
        EffectiveRequestJson(&scratch, &effective.ModelInfo,
                             &effective.Options, &effective.Context);
    started.GenerationStarted.MaxTokens = effective.Options.MaxTokens;
    started.GenerationStarted.ProvisionedAssistantEntryId = assistantId;
    SessionResult persisted = AgentHarness_Append(harness, &started, 1);
    if (!SessionResult_IsOk(persisted)) {
      ArenaRelease(&scratch);
      return persisted;
    }

    StreamOptions options = effective.Options;
    options.Abort = &harness->AbortFlag;
    AgentProviderAttemptInput input = {};
    input.Arena = &harness->Work;
    input.ProviderImpl = harness->Config.ProviderImpl;
    input.ModelInfo = &effective.ModelInfo;
    input.Context = &effective.Context;
    input.Options = options;
    HarnessStreamBridge bridge = {};
    bridge.Harness = harness;
    bridge.Downstream = harness->Config.StreamEvents;
    bridge.OperationId = operationId;
    bridge.AttemptId = attemptId;
    input.Sink.User = &bridge;
    input.Sink.OnEvent = HarnessStreamEvent;
    input.AuthUser = harness->Config.AuthUser;
    input.ResolveAuth = harness->Config.ResolveAuth;
    AgentProviderAttemptResult attempt = AgentProvider_RunAttempt(&input);
    if (harness->Config.Hooks.AfterProvider) {
      SessionResult hooked = HookFailure(
          harness->Config.Hooks.AfterProvider(
              harness->Config.Hooks.User, &harness->Work,
              OperationKind::Run, attemptIndex, &attempt.Assistant),
          "after-provider hook failed");
      if (!SessionResult_IsOk(hooked)) {
        SessionRecord failed =
            NewRecord(SessionRecordKind::GenerationFinished);
        failed.GenerationFinished.OperationId = operationId;
        failed.GenerationFinished.AttemptId = attemptId;
        failed.GenerationFinished.Outcome = GenerationOutcome::Failed;
        failed.GenerationFinished.Error = hooked.Message;
        SessionResult closed = AgentHarness_Append(harness, &failed, 1);
        ArenaRelease(&scratch);
        return SessionResult_IsOk(closed) ? hooked : closed;
      }
    }

    AgentRetryDecision retry = AgentProvider_RetryDecision(
        &harness->Config.Retry, attemptIndex, attempt.Ok, &attempt.Assistant);
    if (retry.Retry) {
      SessionRecord failed = NewRecord(SessionRecordKind::GenerationFinished);
      failed.GenerationFinished.OperationId = operationId;
      failed.GenerationFinished.AttemptId = attemptId;
      failed.GenerationFinished.Outcome = GenerationOutcome::Failed;
      failed.GenerationFinished.Error = attempt.Assistant.ErrorMessage;
      SessionResult closed = AgentHarness_Append(harness, &failed, 1);
      ArenaRelease(&scratch);
      if (!SessionResult_IsOk(closed)) return closed;
      if (AgentProvider_SleepBackoff(retry.DelayMs, &harness->AbortFlag))
        return SessionResult_Error(SessionStatus::Cancelled,
                                   SVL("operation cancelled during retry"));
      continue;
    }

    SessionRecord resultRecords[2] = {};
    resultRecords[0] = NewRecord(SessionRecordKind::Message);
    resultRecords[0].EntryId = assistantId;
    resultRecords[0].MessageEntry.ParentId = *ioHead;
    resultRecords[0].MessageEntry.Value = attempt.Assistant;
    resultRecords[1] = NewRecord(SessionRecordKind::GenerationFinished);
    resultRecords[1].GenerationFinished.OperationId = operationId;
    resultRecords[1].GenerationFinished.AttemptId = attemptId;
    resultRecords[1].GenerationFinished.Outcome =
        attempt.Assistant.Stop == StopReason::Aborted
            ? GenerationOutcome::Aborted
            : ((attempt.Ok && attempt.Assistant.Stop != StopReason::Error)
                   ? GenerationOutcome::Succeeded
                   : GenerationOutcome::Failed);
    resultRecords[1].GenerationFinished.MessageEntryId = assistantId;
    resultRecords[1].GenerationFinished.Error = attempt.Assistant.ErrorMessage;
    SessionResult committed = AgentHarness_Append(harness, resultRecords, 2);
    ArenaRelease(&scratch);
    if (!SessionResult_IsOk(committed)) return committed;
    *ioHead = assistantId;
    *outAssistant = attempt.Assistant;
    *outProviderOk = attempt.Ok;
    return committed;
  }
}

SessionResult RunTools(AgentHarness* harness, DurableId operationId,
                       DurableId* ioHead, const Message* assistant,
                       bool* outAnyTool, bool* outAllTerminate) {
  Message effectiveAssistant = *assistant;
  effectiveAssistant.Content =
      Array_Make<Content>(&harness->Work, assistant->Content.Count);
  SessionResult beforeToolFailure = SessionResult_Ok();
  for (u32 i = 0; i < assistant->Content.Count; i++) {
    Content content = assistant->Content.Data[i];
    if (content.Kind == ContentKind::ToolCall &&
        harness->Config.Hooks.BeforeTool &&
        SessionResult_IsOk(beforeToolFailure)) {
      StrView arguments = content.ToolArgsJson;
      SessionResult hooked = HookFailure(
          harness->Config.Hooks.BeforeTool(
              harness->Config.Hooks.User, &harness->Work,
              content.ToolName, &arguments),
          "before-tool hook failed");
      if (!SessionResult_IsOk(hooked)) {
        beforeToolFailure = hooked;
      } else if (arguments.Length == 0) {
        beforeToolFailure = SessionResult_Error(
            SessionStatus::HookError,
            SVL("before-tool hook produced empty arguments"));
      } else {
        content.ToolArgsJson = StrView_Dup(&harness->Work, arguments);
      }
    }
    Array_Push(&effectiveAssistant.Content, content);
  }
  AgentToolBatchPlan plan = {};
  AgentToolBatch_Prepare(
      &harness->Work, harness->Config.Tools, harness->Config.ToolCount,
      &effectiveAssistant,
      harness->Config.ToolExec, &harness->AbortFlag, &harness->Config.ToolHooks,
      &plan);
  if (!SessionResult_IsOk(beforeToolFailure)) {
    for (u32 i = 0; i < plan.Calls.Count; i++) {
      plan.Calls.Data[i].Runnable = false;
      plan.Calls.Data[i].Result =
          Array_Make<Content>(&harness->Work, 1);
      Array_Push(&plan.Calls.Data[i].Result,
                 Content_Text(beforeToolFailure.Message));
      plan.Calls.Data[i].IsError = true;
      plan.Calls.Data[i].Terminate = false;
    }
    plan.RunnableCount = 0;
  }
  if (plan.Calls.Count == 0) {
    *outAnyTool = false;
    *outAllTerminate = true;
    return SessionResult_Ok();
  }

  Arena scratch = ArenaCreate();
  Array<SessionRecord> starts =
      Array_Make<SessionRecord>(&scratch, plan.Calls.Count);
  Array<DurableId> resultIds =
      Array_Make<DurableId>(&scratch, plan.Calls.Count);
  for (u32 i = 0; i < plan.Calls.Count; i++) {
    AgentPreparedTool* prepared = &plan.Calls.Data[i];
    DurableId resultId = DurableId_Generate();
    Array_Push(&resultIds, resultId);
    SessionRecord started = NewRecord(SessionRecordKind::ToolStarted);
    started.ToolStarted.OperationId = operationId;
    started.ToolStarted.ToolExecutionId = DurableId_Generate();
    started.ToolStarted.ToolCallId = prepared->Call->ToolId;
    started.ToolStarted.ToolName = prepared->Call->ToolName;
    started.ToolStarted.EffectiveArgumentsJson = prepared->Call->ToolArgsJson;
    started.ToolStarted.ProvisionedResultEntryId = resultId;
    started.ToolStarted.ReplayPolicy =
        prepared->Resolved ? prepared->Resolved->ReplayPolicy
                           : ToolReplayPolicy::ManualReconcile;
    Array_Push(&starts, started);
  }
  SessionResult persisted =
      AgentHarness_Append(harness, starts.Data, starts.Count);
  if (!SessionResult_IsOk(persisted)) {
    ArenaRelease(&scratch);
    return persisted;
  }

  AgentToolBatch_Run(&plan, harness->Config.MaxParallelTools);
  AgentToolBatchResult results = {};
  AgentToolBatch_Finalize(&harness->Work, &plan, &harness->Config.ToolHooks,
                          &results);
  Array<SessionRecord> finishes =
      Array_Make<SessionRecord>(&scratch, results.Calls.Count * 2);
  DurableId head = *ioHead;
  for (u32 i = 0; i < results.Calls.Count; i++) {
    SessionRecord message = NewRecord(SessionRecordKind::Message);
    message.EntryId = resultIds.Data[i];
    message.MessageEntry.ParentId = head;
    message.MessageEntry.Value =
        AgentToolResult_Materialize(&results.Calls.Data[i]);
    Array_Push(&finishes, message);
    head = message.EntryId;

    SessionRecord finished = NewRecord(SessionRecordKind::ToolFinished);
    finished.ToolFinished.OperationId = operationId;
    finished.ToolFinished.ToolExecutionId =
        starts.Data[i].ToolStarted.ToolExecutionId;
    finished.ToolFinished.Outcome = results.Calls.Data[i].IsError
                                        ? ToolOutcomeKind::Failed
                                        : ToolOutcomeKind::Succeeded;
    finished.ToolFinished.ResultEntryId = message.EntryId;
    finished.ToolFinished.Terminate = results.Calls.Data[i].Terminate;
    Array_Push(&finishes, finished);
  }
  SessionResult committed =
      AgentHarness_Append(harness, finishes.Data, finishes.Count);
  if (SessionResult_IsOk(committed)) {
    *ioHead = head;
    *outAnyTool = results.AnyTool;
    *outAllTerminate = results.AllTerminate;
  }
  ArenaRelease(&scratch);
  return SessionResult_IsOk(committed) &&
                 !SessionResult_IsOk(beforeToolFailure)
             ? SessionResult_Error(SessionStatus::HookError,
                                   beforeToolFailure.Message, committed.Seq)
             : committed;
}

SessionResult ContinueRun(AgentHarness* harness, DurableId operationId,
                          DurableId head);

SessionResult ContinueAfterTools(AgentHarness* harness, DurableId operationId,
                                 DurableId head, bool anyTool,
                                 bool allTerminate) {
  u32 steering = 0;
  SessionResult steered = CommitQueueCheckpoint(
      harness, QueueKind::Steering, operationId, &head, &steering);
  if (!SessionResult_IsOk(steered)) return steered;
  if (AgentCheckpoint_ContinueTurn(anyTool, allTerminate, steering))
    return ContinueRun(harness, operationId, head);

  u32 followUp = 0;
  SessionResult followed = CommitQueueCheckpoint(
      harness, QueueKind::FollowUp, operationId, &head, &followUp);
  if (!SessionResult_IsOk(followed)) return followed;
  if (AgentCheckpoint_ContinueRun(followUp))
    return ContinueRun(harness, operationId, head);

  return FinishOperation(harness, operationId, head,
                         OperationOutcome::Succeeded, StrView{});
}

SessionResult RecoverPendingTools(AgentHarness* harness, DurableId operationId,
                                  DurableId* ioHead, bool* outAnyTool,
                                  bool* outAllTerminate) {
  Arena scratch = ArenaCreate();
  Array<SessionRecord> records = {};
  SessionReduction reduction = {};
  SessionResult loaded = Load(harness, &scratch, &records, &reduction);
  if (!SessionResult_IsOk(loaded)) {
    ArenaRelease(&scratch);
    return loaded;
  }
  if (reduction.PendingTools.Count == 0) {
    ArenaRelease(&scratch);
    return SessionResult_Error(SessionStatus::InvalidState,
                               SVL("no interrupted tools are pending"));
  }

  Message synthetic = Message_AssistantEmpty(&harness->Work);
  synthetic.Stop = StopReason::ToolUse;
  Array<const SessionRecord*> recoveries =
      Array_Make<const SessionRecord*>(&scratch, reduction.PendingTools.Count);
  Array<const SessionRecord*> existingResults =
      Array_Make<const SessionRecord*>(&scratch, reduction.PendingTools.Count);
  for (u32 i = 0; i < reduction.PendingTools.Count; i++) {
    const SessionRecord* pending = reduction.PendingTools.Data[i];
    const SessionRecord* recovery =
        LatestRecovery(&records, pending->ToolStarted.ToolExecutionId);
    const SessionRecord* existing =
        FindEntry(&records, pending->ToolStarted.ProvisionedResultEntryId);
    if (existing &&
        (existing->Kind != SessionRecordKind::Message ||
         existing->MessageEntry.Value.Kind != Role::ToolResult)) {
      ArenaRelease(&scratch);
      return SessionResult_Error(
          SessionStatus::Corrupt,
          SVL("provisioned tool result is not a tool-result message"));
    }
    if (pending->ToolStarted.ReplayPolicy == ToolReplayPolicy::ManualReconcile &&
        !recovery && !existing) {
      ArenaRelease(&scratch);
      return SessionResult_Error(SessionStatus::RecoveryRequired,
                                 SVL("manual tool reconciliation is required"));
    }
    Array_Push(&recoveries, recovery);
    Array_Push(&existingResults, existing);
    Array_Push(
        &synthetic.Content,
        Content_ToolCall(
            StrView_Dup(&harness->Work, pending->ToolStarted.ToolCallId),
            StrView_Dup(&harness->Work, pending->ToolStarted.ToolName),
            StrView_Dup(&harness->Work,
                        pending->ToolStarted.EffectiveArgumentsJson)));
  }

  AgentToolHooks prepareHooks = {};
  AgentToolBatchPlan plan = {};
  AgentToolBatch_Prepare(
      &harness->Work, harness->Config.Tools, harness->Config.ToolCount,
      &synthetic, harness->Config.ToolExec, &harness->AbortFlag, &prepareHooks,
      &plan);

  Array<SessionRecord> replayIntents =
      Array_Make<SessionRecord>(&scratch, reduction.PendingTools.Count);
  for (u32 i = 0; i < plan.Calls.Count; i++) {
    const SessionRecord* pending = reduction.PendingTools.Data[i];
    const SessionRecord* recovery = recoveries.Data[i];
    const SessionRecord* existing = existingResults.Data[i];
    if (existing) {
      plan.Calls.Data[i].Runnable = false;
      plan.Calls.Data[i].Result = existing->MessageEntry.Value.Content;
      plan.Calls.Data[i].IsError = existing->MessageEntry.Value.IsError;
      plan.Calls.Data[i].Terminate = false;
    } else if (recovery &&
        recovery->RecoveryResolved.Resolution ==
            RecoveryResolution::RecordFailure) {
      plan.Calls.Data[i].Runnable = false;
      plan.Calls.Data[i].Result = Array_Make<Content>(&harness->Work, 1);
      Array_Push(&plan.Calls.Data[i].Result,
                 Content_Text(SVL("Tool effect was interrupted; recorded as failed")));
      plan.Calls.Data[i].IsError = true;
      plan.Calls.Data[i].Terminate = false;
    } else if (recovery &&
               recovery->RecoveryResolved.Resolution ==
                   RecoveryResolution::SupplyResult) {
      plan.Calls.Data[i].Runnable = false;
      plan.Calls.Data[i].Result =
          recovery->RecoveryResolved.SuppliedResult.Content;
      plan.Calls.Data[i].IsError =
          recovery->RecoveryResolved.SuppliedResult.IsError;
      plan.Calls.Data[i].Terminate = false;
    } else if (!recovery) {
      SessionRecord intent = NewRecord(SessionRecordKind::RecoveryResolved);
      intent.RecoveryResolved.OperationId = operationId;
      intent.RecoveryResolved.ToolExecutionId =
          pending->ToolStarted.ToolExecutionId;
      intent.RecoveryResolved.Resolution = RecoveryResolution::RerunAnyway;
      Array_Push(&replayIntents, intent);
    }
  }
  plan.RunnableCount = 0;
  for (u32 i = 0; i < plan.Calls.Count; i++)
    if (plan.Calls.Data[i].Runnable) plan.RunnableCount++;
  if (replayIntents.Count) {
    SessionResult persisted = AgentHarness_Append(
        harness, replayIntents.Data, replayIntents.Count);
    if (!SessionResult_IsOk(persisted)) {
      ArenaRelease(&scratch);
      return persisted;
    }
  }

  AgentToolBatch_Run(&plan, harness->Config.MaxParallelTools);
  AgentToolHooks finalizeHooks = {};
  finalizeHooks.User = harness->Config.ToolHooks.User;
  finalizeHooks.AfterTool = harness->Config.ToolHooks.AfterTool;
  AgentToolBatchResult results = {};
  AgentToolBatch_Finalize(&harness->Work, &plan, &finalizeHooks, &results);

  Array<SessionRecord> finishes =
      Array_Make<SessionRecord>(&scratch, results.Calls.Count * 2);
  DurableId head = *ioHead;
  bool allTerminate = results.AnyTool;
  for (u32 i = 0; i < results.Calls.Count; i++) {
    const SessionRecord* pending = reduction.PendingTools.Data[i];
    const SessionRecord* recovery = recoveries.Data[i];
    const SessionRecord* existing = existingResults.Data[i];
    Message resultMessage = AgentToolResult_Materialize(&results.Calls.Data[i]);
    if (existing) {
      resultMessage = existing->MessageEntry.Value;
    } else if (recovery &&
        recovery->RecoveryResolved.Resolution ==
            RecoveryResolution::SupplyResult)
      resultMessage = recovery->RecoveryResolved.SuppliedResult;

    DurableId resultEntryId = pending->ToolStarted.ProvisionedResultEntryId;
    if (!existing) {
      SessionRecord message = NewRecord(SessionRecordKind::Message);
      message.EntryId = resultEntryId;
      message.MessageEntry.ParentId = head;
      message.MessageEntry.Value = resultMessage;
      Array_Push(&finishes, message);
      head = message.EntryId;
    }

    SessionRecord finished = NewRecord(SessionRecordKind::ToolFinished);
    finished.ToolFinished.OperationId = operationId;
    finished.ToolFinished.ToolExecutionId =
        pending->ToolStarted.ToolExecutionId;
    finished.ToolFinished.Outcome = resultMessage.IsError
                                        ? ToolOutcomeKind::Failed
                                        : ToolOutcomeKind::Succeeded;
    finished.ToolFinished.ResultEntryId = resultEntryId;
    finished.ToolFinished.Terminate =
        existing ? false : results.Calls.Data[i].Terminate;
    if (!finished.ToolFinished.Terminate) allTerminate = false;
    Array_Push(&finishes, finished);
  }
  SessionResult committed =
      AgentHarness_Append(harness, finishes.Data, finishes.Count);
  if (SessionResult_IsOk(committed)) {
    *ioHead = head;
    *outAnyTool = true;
    *outAllTerminate = allTerminate;
  }
  ArenaRelease(&scratch);
  return committed;
}

SessionResult ContinueRun(AgentHarness* harness, DurableId operationId,
                          DurableId head) {
  for (;;) {
    if (harness->AbortFlag) {
      SessionResult finished = FinishOperation(
          harness, operationId, head, OperationOutcome::Cancelled,
          SVL("operation cancelled"));
      return SessionResult_IsOk(finished)
                 ? SessionResult_Error(SessionStatus::Cancelled,
                                       SVL("operation cancelled"), finished.Seq)
                 : finished;
    }

    Message assistant = {};
    bool providerOk = false;
    SessionResult generated =
        RunProvider(harness, operationId, &head, &assistant, &providerOk);
    if (generated.Status == SessionStatus::Cancelled) {
      SessionResult finished = FinishOperation(
          harness, operationId, head, OperationOutcome::Cancelled,
          SVL("operation cancelled"));
      return SessionResult_IsOk(finished)
                 ? SessionResult_Error(SessionStatus::Cancelled,
                                       SVL("operation cancelled"), finished.Seq)
                 : finished;
    }
    if (generated.Status == SessionStatus::HookError) {
      SessionResult finished = FinishOperation(
          harness, operationId, head, OperationOutcome::Failed,
          generated.Message);
      return SessionResult_IsOk(finished) ? generated : finished;
    }
    if (!SessionResult_IsOk(generated)) return generated;
    if (!providerOk || assistant.Stop == StopReason::Error ||
        assistant.Stop == StopReason::Aborted) {
      OperationOutcome outcome = assistant.Stop == StopReason::Aborted
                                     ? OperationOutcome::Cancelled
                                     : OperationOutcome::Failed;
      SessionResult finished = FinishOperation(
          harness, operationId, head, outcome, assistant.ErrorMessage);
      if (!SessionResult_IsOk(finished)) return finished;
      return SessionResult_Error(
          outcome == OperationOutcome::Cancelled ? SessionStatus::Cancelled
                                                 : SessionStatus::ProviderError,
          assistant.ErrorMessage, finished.Seq);
    }

    bool anyTool = false;
    bool allTerminate = true;
    SessionResult tools = RunTools(harness, operationId, &head, &assistant,
                                   &anyTool, &allTerminate);
    if (tools.Status == SessionStatus::HookError) {
      SessionResult finished = FinishOperation(
          harness, operationId, head, OperationOutcome::Failed,
          tools.Message);
      return SessionResult_IsOk(finished) ? tools : finished;
    }
    if (!SessionResult_IsOk(tools)) return tools;
    return ContinueAfterTools(harness, operationId, head, anyTool,
                              allTerminate);
  }
}

bool CompletedToolBatchState(const Array<SessionRecord>* records,
                             const SessionReduction* reduction,
                             bool* outAnyTool, bool* outAllTerminate) {
  i32 assistantIndex = -1;
  for (u32 i = reduction->Branch.Count; i > 0; i--) {
    const SessionRecord* record = reduction->Branch.Data[i - 1];
    if (record->Kind == SessionRecordKind::Message &&
        record->MessageEntry.Value.Kind == Role::Assistant) {
      assistantIndex = (i32)(i - 1);
      break;
    }
  }
  if (assistantIndex < 0) return false;

  bool any = false;
  bool allTerminate = true;
  for (u32 i = (u32)assistantIndex + 1; i < reduction->Branch.Count; i++) {
    const SessionRecord* message = reduction->Branch.Data[i];
    if (message->Kind != SessionRecordKind::Message ||
        message->MessageEntry.Value.Kind != Role::ToolResult)
      continue;
    any = true;
    const SessionRecord* finished = nullptr;
    for (u32 j = 0; j < records->Count; j++) {
      if (records->Data[j].Kind == SessionRecordKind::ToolFinished &&
          DurableId_Eq(records->Data[j].ToolFinished.ResultEntryId,
                       message->EntryId)) {
        finished = &records->Data[j];
        break;
      }
    }
    if (!finished) return false;
    if (!finished->ToolFinished.Terminate) allTerminate = false;
  }
  *outAnyTool = any;
  *outAllTerminate = any && allTerminate;
  return any;
}

StrView ExtractSummaryText(Arena* arena, const Message* assistant) {
  StrBuf text = StrBuf_Make(arena, 256);
  for (u32 i = 0; i < assistant->Content.Count; i++) {
    const Content* content = &assistant->Content.Data[i];
    if ((content->Kind == ContentKind::Text ||
         content->Kind == ContentKind::Thinking) &&
        content->Text.Length) {
      if (text.Length) StrBuf_AppendChar(&text, '\n');
      StrBuf_Append(&text, content->Text);
    }
  }
  return StrBuf_View(&text);
}

SessionResult ContinueCompaction(AgentHarness* harness, DurableId operationId,
                                 DurableId startingHeadId,
                                 DurableId compactionEntryId) {
  for (u32 attemptIndex = 0;; attemptIndex++) {
    Arena scratch = ArenaCreate();
    Array<SessionRecord> records = {};
    SessionReduction reduction = {};
    SessionResult loaded = Load(harness, &scratch, &records, &reduction);
    if (!SessionResult_IsOk(loaded)) {
      ArenaRelease(&scratch);
      return loaded;
    }
    Array<Message> contextMessages = {};
    SessionResult contextResult = SessionReducer_Context(
        &scratch, &reduction, startingHeadId, &contextMessages);
    if (!SessionResult_IsOk(contextResult)) {
      ArenaRelease(&scratch);
      return contextResult;
    }
    if (harness->Config.Hooks.BeforeContext) {
      SessionResult hooked = HookFailure(
          harness->Config.Hooks.BeforeContext(
              harness->Config.Hooks.User, &scratch,
              OperationKind::Compaction, startingHeadId, &contextMessages),
          "before-context hook failed");
      if (!SessionResult_IsOk(hooked)) {
        SessionResult finished = FinishOperation(
            harness, operationId, startingHeadId, OperationOutcome::Failed,
            hooked.Message);
        ArenaRelease(&scratch);
        return SessionResult_IsOk(finished) ? hooked : finished;
      }
    }
    Array<Message> requestMessages =
        Array_Make<Message>(&scratch, contextMessages.Count + 1);
    for (u32 i = 0; i < contextMessages.Count; i++)
      Array_Push(&requestMessages, contextMessages.Data[i]);
    Array_Push(
        &requestMessages,
        Message_User(
            &scratch,
            SVL("Summarize the conversation for future continuation. Preserve goals, "
                "decisions, constraints, unresolved work, and important tool results.")));

    AgentProviderRequest request = {};
    AgentProvider_BuildRequest(
        &scratch, harness->Config.SystemPrompt, requestMessages.Data,
        requestMessages.Count, harness->Config.Tools, harness->Config.ToolCount,
        &request);
    DurableId attemptId = DurableId_Generate();
    AgentHarnessProviderInput effective = {};
    effective.ModelInfo = harness->Config.ModelInfo;
    effective.Context = request.Context;
    effective.Options = harness->Config.Stream;
    if (harness->Config.Hooks.BeforeProvider) {
      SessionResult hooked = HookFailure(
          harness->Config.Hooks.BeforeProvider(
              harness->Config.Hooks.User, &scratch,
              OperationKind::Compaction, attemptIndex, &effective),
          "before-provider hook failed");
      if (!SessionResult_IsOk(hooked)) {
        SessionResult finished = FinishOperation(
            harness, operationId, startingHeadId, OperationOutcome::Failed,
            hooked.Message);
        ArenaRelease(&scratch);
        return SessionResult_IsOk(finished) ? hooked : finished;
      }
    }
    if (effective.ModelInfo.Provider.Length == 0 ||
        effective.ModelInfo.Id.Length == 0 ||
        effective.Options.MaxTokens < 0) {
      SessionResult hooked = SessionResult_Error(
          SessionStatus::HookError,
          SVL("before-provider hook produced invalid settings"));
      SessionResult finished = FinishOperation(
          harness, operationId, startingHeadId, OperationOutcome::Failed,
          hooked.Message);
      ArenaRelease(&scratch);
      return SessionResult_IsOk(finished) ? hooked : finished;
    }
    SessionRecord started = NewRecord(SessionRecordKind::GenerationStarted);
    started.GenerationStarted.OperationId = operationId;
    started.GenerationStarted.AttemptId = attemptId;
    started.GenerationStarted.Provider = effective.ModelInfo.Provider;
    started.GenerationStarted.Model = effective.ModelInfo.Id;
    started.GenerationStarted.EffectiveRequestJson =
        EffectiveRequestJson(&scratch, &effective.ModelInfo,
                             &effective.Options, &effective.Context);
    started.GenerationStarted.MaxTokens = effective.Options.MaxTokens;
    started.GenerationStarted.ProvisionedAssistantEntryId = compactionEntryId;
    SessionResult persisted = AgentHarness_Append(harness, &started, 1);
    if (!SessionResult_IsOk(persisted)) {
      ArenaRelease(&scratch);
      return persisted;
    }

    StreamOptions options = effective.Options;
    options.Abort = &harness->AbortFlag;
    AgentProviderAttemptInput input = {};
    input.Arena = &harness->Work;
    input.ProviderImpl = harness->Config.ProviderImpl;
    input.ModelInfo = &effective.ModelInfo;
    input.Context = &effective.Context;
    input.Options = options;
    HarnessStreamBridge bridge = {};
    bridge.Harness = harness;
    bridge.Downstream = harness->Config.StreamEvents;
    bridge.OperationId = operationId;
    bridge.AttemptId = attemptId;
    input.Sink.User = &bridge;
    input.Sink.OnEvent = HarnessStreamEvent;
    input.AuthUser = harness->Config.AuthUser;
    input.ResolveAuth = harness->Config.ResolveAuth;
    AgentProviderAttemptResult attempt = AgentProvider_RunAttempt(&input);
    if (harness->Config.Hooks.AfterProvider) {
      SessionResult hooked = HookFailure(
          harness->Config.Hooks.AfterProvider(
              harness->Config.Hooks.User, &harness->Work,
              OperationKind::Compaction, attemptIndex, &attempt.Assistant),
          "after-provider hook failed");
      if (!SessionResult_IsOk(hooked)) {
        SessionRecord failed =
            NewRecord(SessionRecordKind::GenerationFinished);
        failed.GenerationFinished.OperationId = operationId;
        failed.GenerationFinished.AttemptId = attemptId;
        failed.GenerationFinished.Outcome = GenerationOutcome::Failed;
        failed.GenerationFinished.Error = hooked.Message;
        SessionResult closed = AgentHarness_Append(harness, &failed, 1);
        if (!SessionResult_IsOk(closed)) {
          ArenaRelease(&scratch);
          return closed;
        }
        SessionResult finished = FinishOperation(
            harness, operationId, startingHeadId, OperationOutcome::Failed,
            hooked.Message);
        ArenaRelease(&scratch);
        return SessionResult_IsOk(finished) ? hooked : finished;
      }
    }
    AgentRetryDecision retry = AgentProvider_RetryDecision(
        &harness->Config.Retry, attemptIndex, attempt.Ok, &attempt.Assistant);
    if (retry.Retry) {
      SessionRecord failed = NewRecord(SessionRecordKind::GenerationFinished);
      failed.GenerationFinished.OperationId = operationId;
      failed.GenerationFinished.AttemptId = attemptId;
      failed.GenerationFinished.Outcome = GenerationOutcome::Failed;
      failed.GenerationFinished.Error = attempt.Assistant.ErrorMessage;
      SessionResult closed = AgentHarness_Append(harness, &failed, 1);
      ArenaRelease(&scratch);
      if (!SessionResult_IsOk(closed)) return closed;
      if (AgentProvider_SleepBackoff(retry.DelayMs, &harness->AbortFlag)) {
        SessionResult finished = FinishOperation(
            harness, operationId, startingHeadId, OperationOutcome::Cancelled,
            SVL("compaction cancelled"));
        return SessionResult_IsOk(finished)
                   ? SessionResult_Error(SessionStatus::Cancelled,
                                         SVL("compaction cancelled"), finished.Seq)
                   : finished;
      }
      continue;
    }

    bool success = attempt.Ok && attempt.Assistant.Stop != StopReason::Error &&
                   attempt.Assistant.Stop != StopReason::Aborted;
    StrView summaryText =
        success ? ExtractSummaryText(&harness->Work, &attempt.Assistant)
                : StrView{};
    if (!success || summaryText.Length == 0) {
      SessionRecord failed = NewRecord(SessionRecordKind::GenerationFinished);
      failed.GenerationFinished.OperationId = operationId;
      failed.GenerationFinished.AttemptId = attemptId;
      failed.GenerationFinished.Outcome =
          attempt.Assistant.Stop == StopReason::Aborted
              ? GenerationOutcome::Aborted
              : GenerationOutcome::Failed;
      failed.GenerationFinished.Error =
          summaryText.Length == 0 && success
              ? SVL("compaction provider returned an empty summary")
              : attempt.Assistant.ErrorMessage;
      SessionResult closed = AgentHarness_Append(harness, &failed, 1);
      if (!SessionResult_IsOk(closed)) {
        ArenaRelease(&scratch);
        return closed;
      }
      OperationOutcome outcome =
          attempt.Assistant.Stop == StopReason::Aborted
              ? OperationOutcome::Cancelled
              : OperationOutcome::Failed;
      StrView error = failed.GenerationFinished.Error;
      SessionResult finished =
          FinishOperation(harness, operationId, startingHeadId, outcome, error);
      ArenaRelease(&scratch);
      if (!SessionResult_IsOk(finished)) return finished;
      return SessionResult_Error(
          outcome == OperationOutcome::Cancelled ? SessionStatus::Cancelled
                                                 : SessionStatus::ProviderError,
          error, finished.Seq);
    }

    StrBuf wrapped = StrBuf_Make(&harness->Work, summaryText.Length + 48);
    StrBuf_AppendCStr(&wrapped, "Summary of previous conversation:\n");
    StrBuf_Append(&wrapped, summaryText);
    Message summary =
        Message_User(&harness->Work, StrBuf_View(&wrapped));
    SessionRecord result[2] = {};
    result[0] = NewRecord(SessionRecordKind::Compaction);
    result[0].EntryId = compactionEntryId;
    result[0].CompactionEntry.ParentId = startingHeadId;
    result[0].CompactionEntry.BaseEntryId =
        reduction.Branch.Data[0]->EntryId;
    result[0].CompactionEntry.ThroughEntryId = startingHeadId;
    result[0].CompactionEntry.SourceHeadId = startingHeadId;
    result[0].CompactionEntry.Summary = summary;
    result[1] = NewRecord(SessionRecordKind::GenerationFinished);
    result[1].GenerationFinished.OperationId = operationId;
    result[1].GenerationFinished.AttemptId = attemptId;
    result[1].GenerationFinished.Outcome = GenerationOutcome::Succeeded;
    result[1].GenerationFinished.MessageEntryId = compactionEntryId;
    SessionResult committed = AgentHarness_Append(harness, result, 2);
    ArenaRelease(&scratch);
    if (!SessionResult_IsOk(committed)) return committed;
    return FinishOperation(harness, operationId, compactionEntryId,
                           OperationOutcome::Succeeded, StrView{});
  }
}

}  // namespace

SessionResult AgentDriver_StartRun(AgentHarness* harness,
                                   const Message* prompt,
                                   DurableId operationId,
                                   DurableId startingHeadId) {
  if (!prompt || prompt->Kind != Role::User)
    return SessionResult_Error(SessionStatus::InvalidArgument,
                               SVL("run prompt must be a user message"));
  DurableId promptId = DurableId_Generate();
  SessionRecord started = NewRecord(SessionRecordKind::OperationStarted);
  started.OperationStarted.OperationId = operationId;
  started.OperationStarted.Operation = OperationKind::Run;
  started.OperationStarted.StartingHeadId = startingHeadId;
  started.OperationStarted.HasAcceptedMessage = true;
  started.OperationStarted.AcceptedMessage = *prompt;
  started.OperationStarted.ProvisionedEntryId = promptId;
  SessionResult accepted = AgentHarness_Append(harness, &started, 1);
  if (!SessionResult_IsOk(accepted)) return accepted;

  DurableId head = startingHeadId;
  u32 nextTurn = 0;
  SessionResult next = CommitQueueCheckpoint(
      harness, QueueKind::NextTurn, operationId, &head, &nextTurn);
  if (!SessionResult_IsOk(next)) return next;

  SessionRecord user = NewRecord(SessionRecordKind::Message);
  user.EntryId = promptId;
  user.MessageEntry.ParentId = head;
  user.MessageEntry.Value = started.OperationStarted.AcceptedMessage;
  SessionResult prompted = AgentHarness_Append(harness, &user, 1);
  if (!SessionResult_IsOk(prompted)) return prompted;
  head = promptId;

  u32 steering = 0;
  SessionResult steered = CommitQueueCheckpoint(
      harness, QueueKind::Steering, operationId, &head, &steering);
  if (!SessionResult_IsOk(steered)) return steered;
  return ContinueRun(harness, operationId, head);
}

SessionResult AgentDriver_ResumeRun(AgentHarness* harness,
                                    const SessionReduction* restored) {
  (void)restored;
  Arena scratch = ArenaCreate();
  Array<SessionRecord> records = {};
  SessionReduction reduction = {};
  SessionResult loaded = Load(harness, &scratch, &records, &reduction);
  if (!SessionResult_IsOk(loaded)) {
    ArenaRelease(&scratch);
    return loaded;
  }
  if (!reduction.HasActiveOperation || !reduction.ActiveOperationStart ||
      reduction.ActiveOperationKind != OperationKind::Run) {
    ArenaRelease(&scratch);
    return SessionResult_Error(SessionStatus::InvalidState,
                               SVL("suspended operation is not a run"));
  }

  DurableId operationId = reduction.ActiveOperationId;
  DurableId head = reduction.HeadId;
  const OperationStartedData* operation =
      &reduction.ActiveOperationStart->OperationStarted;
  if (operation->HasAcceptedMessage &&
      !FindEntry(&records, operation->ProvisionedEntryId)) {
    u32 nextTurn = 0;
    SessionResult next = CommitQueueCheckpoint(
        harness, QueueKind::NextTurn, operationId, &head, &nextTurn);
    if (!SessionResult_IsOk(next)) {
      ArenaRelease(&scratch);
      return next;
    }
    SessionRecord prompt = NewRecord(SessionRecordKind::Message);
    prompt.EntryId = operation->ProvisionedEntryId;
    prompt.MessageEntry.ParentId = head;
    prompt.MessageEntry.Value = operation->AcceptedMessage;
    SessionResult committed = AgentHarness_Append(harness, &prompt, 1);
    if (!SessionResult_IsOk(committed)) {
      ArenaRelease(&scratch);
      return committed;
    }
    head = prompt.EntryId;
    u32 steering = 0;
    SessionResult steered = CommitQueueCheckpoint(
        harness, QueueKind::Steering, operationId, &head, &steering);
    ArenaRelease(&scratch);
    if (!SessionResult_IsOk(steered)) return steered;
    return ContinueRun(harness, operationId, head);
  }

  if (reduction.PendingGeneration) {
    const GenerationStartedData* generation =
        &reduction.PendingGeneration->GenerationStarted;
    const SessionRecord* existing =
        FindEntry(&records, generation->ProvisionedAssistantEntryId);
    if (existing) {
      if (existing->Kind != SessionRecordKind::Message ||
          existing->MessageEntry.Value.Kind != Role::Assistant) {
        ArenaRelease(&scratch);
        return SessionResult_Error(
            SessionStatus::Corrupt,
            SVL("provisioned generation result is not an assistant message"));
      }
      const Message* assistant = &existing->MessageEntry.Value;
      SessionRecord completed =
          NewRecord(SessionRecordKind::GenerationFinished);
      completed.GenerationFinished.OperationId = operationId;
      completed.GenerationFinished.AttemptId = generation->AttemptId;
      completed.GenerationFinished.Outcome =
          assistant->Stop == StopReason::Aborted
              ? GenerationOutcome::Aborted
              : (assistant->Stop == StopReason::Error
                     ? GenerationOutcome::Failed
                     : GenerationOutcome::Succeeded);
      completed.GenerationFinished.MessageEntryId = existing->EntryId;
      completed.GenerationFinished.Error = assistant->ErrorMessage;
      SessionResult closed = AgentHarness_Append(harness, &completed, 1);
      ArenaRelease(&scratch);
      if (!SessionResult_IsOk(closed)) return closed;
      return AgentDriver_ResumeRun(harness, nullptr);
    }
    SessionRecord interrupted =
        NewRecord(SessionRecordKind::GenerationFinished);
    interrupted.GenerationFinished.OperationId = operationId;
    interrupted.GenerationFinished.AttemptId = generation->AttemptId;
    interrupted.GenerationFinished.Outcome = GenerationOutcome::Failed;
    interrupted.GenerationFinished.Error =
        SVL("provider attempt interrupted before a durable result");
    SessionResult closed = AgentHarness_Append(harness, &interrupted, 1);
    ArenaRelease(&scratch);
    if (!SessionResult_IsOk(closed)) return closed;
    return ContinueRun(harness, operationId, head);
  }

  if (reduction.PendingTools.Count) {
    ArenaRelease(&scratch);
    bool anyTool = false;
    bool allTerminate = true;
    SessionResult recovered = RecoverPendingTools(
        harness, operationId, &head, &anyTool, &allTerminate);
    if (!SessionResult_IsOk(recovered)) return recovered;
    return ContinueAfterTools(harness, operationId, head, anyTool,
                              allTerminate);
  }

  const SessionRecord* lastMessage = nullptr;
  for (u32 i = reduction.Branch.Count; i > 0; i--) {
    if (reduction.Branch.Data[i - 1]->Kind == SessionRecordKind::Message) {
      lastMessage = reduction.Branch.Data[i - 1];
      break;
    }
  }
  if (!lastMessage) {
    ArenaRelease(&scratch);
    return SessionResult_Error(SessionStatus::Corrupt,
                               SVL("run operation has no committed message"));
  }

  Message message = lastMessage->MessageEntry.Value;
  if (message.Kind == Role::Assistant) {
    if (message.Stop == StopReason::Error ||
        message.Stop == StopReason::Aborted) {
      OperationOutcome outcome = message.Stop == StopReason::Aborted
                                     ? OperationOutcome::Cancelled
                                     : OperationOutcome::Failed;
      SessionResult finished = FinishOperation(
          harness, operationId, head, outcome, message.ErrorMessage);
      ArenaRelease(&scratch);
      if (!SessionResult_IsOk(finished)) return finished;
      return SessionResult_Error(
          outcome == OperationOutcome::Cancelled ? SessionStatus::Cancelled
                                                 : SessionStatus::ProviderError,
          message.ErrorMessage, finished.Seq);
    }
    bool anyTool = false;
    bool allTerminate = true;
    SessionResult tools = RunTools(harness, operationId, &head, &message,
                                   &anyTool, &allTerminate);
    ArenaRelease(&scratch);
    if (tools.Status == SessionStatus::HookError) {
      SessionResult finished = FinishOperation(
          harness, operationId, head, OperationOutcome::Failed,
          tools.Message);
      return SessionResult_IsOk(finished) ? tools : finished;
    }
    if (!SessionResult_IsOk(tools)) return tools;
    return ContinueAfterTools(harness, operationId, head, anyTool,
                              allTerminate);
  }

  if (message.Kind == Role::ToolResult) {
    bool anyTool = false;
    bool allTerminate = false;
    if (!CompletedToolBatchState(&records, &reduction, &anyTool,
                                 &allTerminate)) {
      ArenaRelease(&scratch);
      return SessionResult_Error(SessionStatus::Corrupt,
                                 SVL("completed tool batch is inconsistent"));
    }
    ArenaRelease(&scratch);
    return ContinueAfterTools(harness, operationId, head, anyTool,
                              allTerminate);
  }

  u32 steering = 0;
  SessionResult steered = CommitQueueCheckpoint(
      harness, QueueKind::Steering, operationId, &head, &steering);
  ArenaRelease(&scratch);
  if (!SessionResult_IsOk(steered)) return steered;
  return ContinueRun(harness, operationId, head);
}

SessionResult AgentDriver_Compact(AgentHarness* harness, DurableId operationId,
                                  DurableId startingHeadId,
                                  DurableId compactionEntryId) {
  SessionRecord started = NewRecord(SessionRecordKind::OperationStarted);
  started.OperationStarted.OperationId = operationId;
  started.OperationStarted.Operation = OperationKind::Compaction;
  started.OperationStarted.StartingHeadId = startingHeadId;
  started.OperationStarted.ProvisionedEntryId = compactionEntryId;
  SessionResult accepted = AgentHarness_Append(harness, &started, 1);
  if (!SessionResult_IsOk(accepted)) return accepted;
  return ContinueCompaction(harness, operationId, startingHeadId,
                            compactionEntryId);
}

SessionResult AgentDriver_ResumeCompaction(AgentHarness* harness,
                                           const SessionReduction* restored) {
  if (!restored || !restored->ActiveOperationStart ||
      restored->ActiveOperationKind != OperationKind::Compaction)
    return SessionResult_Error(SessionStatus::InvalidState,
                               SVL("suspended operation is not compaction"));
  const OperationStartedData* operation =
      &restored->ActiveOperationStart->OperationStarted;

  Arena scratch = ArenaCreate();
  Array<SessionRecord> records = {};
  SessionReduction reduction = {};
  SessionResult loaded = Load(harness, &scratch, &records, &reduction);
  if (!SessionResult_IsOk(loaded)) {
    ArenaRelease(&scratch);
    return loaded;
  }
  const SessionRecord* existing =
      FindEntry(&records, operation->ProvisionedEntryId);
  if (existing) {
    if (existing->Kind != SessionRecordKind::Compaction) {
      ArenaRelease(&scratch);
      return SessionResult_Error(
          SessionStatus::Corrupt,
          SVL("provisioned compaction result is not a compaction entry"));
    }
    if (reduction.PendingGeneration) {
      SessionRecord completed =
          NewRecord(SessionRecordKind::GenerationFinished);
      completed.GenerationFinished.OperationId = operation->OperationId;
      completed.GenerationFinished.AttemptId =
          reduction.PendingGeneration->GenerationStarted.AttemptId;
      completed.GenerationFinished.Outcome = GenerationOutcome::Succeeded;
      completed.GenerationFinished.MessageEntryId = existing->EntryId;
      SessionResult closed = AgentHarness_Append(harness, &completed, 1);
      if (!SessionResult_IsOk(closed)) {
        ArenaRelease(&scratch);
        return closed;
      }
    }
    SessionResult finished = FinishOperation(
        harness, operation->OperationId, operation->ProvisionedEntryId,
        OperationOutcome::Succeeded, StrView{});
    ArenaRelease(&scratch);
    return finished;
  }
  if (reduction.PendingGeneration) {
    SessionRecord interrupted =
        NewRecord(SessionRecordKind::GenerationFinished);
    interrupted.GenerationFinished.OperationId = operation->OperationId;
    interrupted.GenerationFinished.AttemptId =
        reduction.PendingGeneration->GenerationStarted.AttemptId;
    interrupted.GenerationFinished.Outcome = GenerationOutcome::Failed;
    interrupted.GenerationFinished.Error =
        SVL("compaction attempt interrupted before a durable result");
    SessionResult closed = AgentHarness_Append(harness, &interrupted, 1);
    if (!SessionResult_IsOk(closed)) {
      ArenaRelease(&scratch);
      return closed;
    }
  }
  DurableId operationId = operation->OperationId;
  DurableId startingHeadId = operation->StartingHeadId;
  DurableId entryId = operation->ProvisionedEntryId;
  ArenaRelease(&scratch);
  return ContinueCompaction(harness, operationId, startingHeadId, entryId);
}

SessionResult AgentDriver_ResumeNavigation(AgentHarness* harness,
                                           const SessionReduction* restored) {
  if (!restored || !restored->ActiveOperationStart ||
      restored->ActiveOperationKind != OperationKind::Navigation)
    return SessionResult_Error(SessionStatus::InvalidState,
                               SVL("suspended operation is not navigation"));
  const OperationStartedData* operation =
      &restored->ActiveOperationStart->OperationStarted;
  SessionRecord finished = NewRecord(SessionRecordKind::OperationFinished);
  finished.OperationFinished.OperationId = operation->OperationId;
  finished.OperationFinished.Outcome = OperationOutcome::Succeeded;
  finished.OperationFinished.ResultHeadId = operation->TargetEntryId;
  return AgentHarness_Append(harness, &finished, 1);
}
