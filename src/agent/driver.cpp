#include "agent/driver.h"

#include "agent/compaction.h"
#include "net/http.h"

namespace {

SessionRecord NewMessageRecord(DurableId parentId, const Message& value) {
  SessionRecord record = {};
  record.EntryId = DurableId_Generate();
  record.Kind = SessionRecordKind::Message;
  record.MessageEntry.ParentId = parentId;
  record.MessageEntry.Value = value;
  return record;
}

bool ResolveProviderInput(AgentHarnessProviderInput* input) {
  if (!input || input->ModelInfo.Provider.Length == 0 || input->ModelInfo.Id.Length == 0 ||
      input->Options.MaxTokens < 0 ||
      !Model_RequestProfileIsValid(&input->ModelInfo, input->Options.Profile))
    return false;
  input->Options.Profile =
      Model_ResolveRequestProfile(&input->ModelInfo, input->Options.Profile);
  return true;
}

// Forwards every provider StreamEvent both to watchers (as a transient
// harness event) and to the caller's own downstream sink (e.g. TUI/REPL
// rendering), exactly once per event, in delivery order.
void OnProviderStreamEvent(void* user, const StreamEvent& event) {
  AgentHarness* harness = (AgentHarness*)user;
  AgentHarness_PublishStream(harness, &event);
  StreamSink_Emit(harness->Config.StreamEvents, event);
}

void OnProviderRequestRetry(void* user, const HttpRetryEvent* retry) {
  AgentHarness* harness = (AgentHarness*)user;
  AgentHarnessEventKind kind = retry->Kind == HttpRetryEventKind::Start
                                   ? AgentHarnessEventKind::TransientRetryStart
                                   : AgentHarnessEventKind::TransientRetryEnd;
  StrView error = retry->Error.Length ? retry->Error : SVL("retryable HTTP response");
  AgentHarness_PublishRetry(harness, kind, RetryLayer::ProviderRequest,
                            retry->Attempt, retry->MaxRetries, retry->DelayMs,
                            error);
}

void OnToolUpdateBridge(void* user, StrView toolCallId, StrView toolName,
                        const ToolResult* partial) {
  AgentHarness_PublishToolUpdate((AgentHarness*)user, toolCallId, toolName, partial);
}

void OnToolEndBridge(void* user, StrView toolCallId, StrView toolName,
                     const ToolResult* result, bool isError) {
  AgentHarness_PublishToolEnd((AgentHarness*)user, toolCallId, toolName, result, isError);
}

AgentToolEventSink ToolEvents(AgentHarness* harness) {
  AgentToolEventSink sink = {};
  sink.User = harness;
  sink.OnUpdate = OnToolUpdateBridge;
  sink.OnEnd = OnToolEndBridge;
  return sink;
}

bool TryStartTool(void* user) {
  AgentHarness* harness = (AgentHarness*)user;
  ScopedLock lock(&harness->StateLock);
  return !harness->AbortFlag;
}

struct TurnRunResult {
  Message Assistant;
  bool ProviderOk;
};

// Builds the active-branch context (linear in branch length, never full
// history), runs the BeforeContext/BeforeProvider/AfterProvider hooks, and
// drives the assistant-turn retry layer (Config.Retry / AgentRetryPolicy),
// emitting transient retry_start/retry_end events around each backoff. Never
// appends anything -- the caller decides what a successful/failed attempt
// means for the tree (a plain assistant message for a run turn, or a
// Compaction entry for compaction).
SessionResult RunAssistantTurn(AgentHarness* harness, AgentTurnKind turnKind, DurableId headId,
                               const Message* extraTrailingMessage, TurnRunResult* out) {
  SessionReduction reduction = AgentHarness_ReduceSnapshot(harness);
  Array<Message> contextMessages = {};
  SessionResult contextResult =
      SessionReducer_Context(&harness->Work, &reduction, headId, &contextMessages);
  if (!SessionResult_IsOk(contextResult)) return contextResult;
  if (extraTrailingMessage) Array_Push(&contextMessages, *extraTrailingMessage);

  if (harness->Config.Hooks.BeforeContext) {
    SessionResult hooked = AgentHook_Failure(
        harness->Config.Hooks.BeforeContext(harness->Config.Hooks.User, &harness->Work,
                                            turnKind, headId, &contextMessages),
        "before-context hook failed");
    if (!SessionResult_IsOk(hooked)) return hooked;
  }

  AgentProviderRequest request = {};
  AgentProvider_BuildRequest(&harness->Work, harness->Config.SystemPrompt, contextMessages.Data,
                             contextMessages.Count, harness->Config.Tools,
                             harness->Config.ToolCount, &request);

  for (u32 attemptIndex = 0;; attemptIndex++) {
    AgentHarnessProviderInput effective = {};
    effective.ModelInfo = harness->Config.ModelInfo;
    effective.Context = request.Context;
    effective.Options = harness->Config.Stream;
    if (harness->Config.Hooks.BeforeProvider) {
      SessionResult hooked = AgentHook_Failure(
          harness->Config.Hooks.BeforeProvider(harness->Config.Hooks.User, &harness->Work,
                                               turnKind, attemptIndex, &effective),
          "before-provider hook failed");
      if (!SessionResult_IsOk(hooked)) return hooked;
    }
    if (!ResolveProviderInput(&effective))
      return SessionResult_Error(SessionStatus::HookError,
                                 SVL("before-provider hook produced invalid settings"));

    StreamOptions options = effective.Options;
    options.Abort = &harness->AbortFlag;
    HttpRetryPolicy providerRetry = {};
    if (options.Retry) {
      providerRetry = *options.Retry;
      providerRetry.EventUser = harness;
      providerRetry.OnEvent = OnProviderRequestRetry;
      options.Retry = &providerRetry;
    }
    AgentProviderAttemptInput input = {};
    input.Arena = &harness->Work;
    input.ProviderImpl = harness->Config.ProviderImpl;
    input.ModelInfo = &effective.ModelInfo;
    input.Context = &effective.Context;
    input.Options = options;
    input.Sink.User = harness;
    input.Sink.OnEvent = OnProviderStreamEvent;
    input.AuthUser = harness->Config.AuthUser;
    input.ResolveAuth = harness->Config.ResolveAuth;
    AgentProviderAttemptResult attempt = AgentProvider_RunAttempt(&input);

    if (harness->Config.Hooks.AfterProvider) {
      SessionResult hooked = AgentHook_Failure(
          harness->Config.Hooks.AfterProvider(harness->Config.Hooks.User, &harness->Work,
                                              turnKind, attemptIndex, &attempt.Assistant),
          "after-provider hook failed");
      if (!SessionResult_IsOk(hooked)) return hooked;
    }

    AgentRetryDecision decision = AgentProvider_RetryDecision(
        &harness->Config.Retry, attemptIndex, attempt.Ok, &attempt.Assistant);
    if (decision.Retry) {
      u32 attemptNumber = attemptIndex + 1;
      AgentHarness_PublishRetry(harness, AgentHarnessEventKind::TransientRetryStart,
                                RetryLayer::AssistantTurn, attemptNumber,
                                harness->Config.Retry.MaxRetries, decision.DelayMs,
                                attempt.Assistant.ErrorMessage);
      bool aborted = AgentProvider_SleepBackoff(decision.DelayMs, &harness->AbortFlag);
      AgentHarness_PublishRetry(harness, AgentHarnessEventKind::TransientRetryEnd,
                                RetryLayer::AssistantTurn, attemptNumber,
                                harness->Config.Retry.MaxRetries, decision.DelayMs,
                                attempt.Assistant.ErrorMessage);
      if (aborted)
        return SessionResult_Error(SessionStatus::Cancelled,
                                   SVL("turn cancelled during retry backoff"));
      continue;
    }

    out->Assistant = attempt.Assistant;
    out->ProviderOk = attempt.Ok;
    return SessionResult_Ok();
  }
}

// Resolves each tool call's effective arguments (BeforeTool hook), runs the
// batch (parallel by default), then appends every tool-result message in
// assistant/source order as one batch. Every prepared call always receives
// exactly one finalized result -- including a live cancellation, which still
// runs each already-dispatched call to a normal (possibly error) completion
// before this function returns -- so the harness never leaves a live tool
// call without a result before the next provider request.
SessionResult RunToolBatch(AgentHarness* harness, DurableId* ioHead, const Message* assistant,
                          bool* outAnyTool, bool* outAllTerminate) {
  Arena* arena = &harness->Work;
  Message effectiveAssistant = *assistant;
  effectiveAssistant.Content =
      Array_Make<Content>(arena, assistant->Content.Count ? assistant->Content.Count : 1);
  SessionResult beforeToolFailure = SessionResult_Ok();
  for (u32 i = 0; i < assistant->Content.Count; i++) {
    Content content = assistant->Content.Data[i];
    if (content.Kind == ContentKind::ToolCall && !harness->AbortFlag &&
        harness->Config.Hooks.BeforeTool &&
        SessionResult_IsOk(beforeToolFailure)) {
      StrView arguments = content.ToolArgsJson;
      SessionResult hooked = AgentHook_Failure(
          harness->Config.Hooks.BeforeTool(harness->Config.Hooks.User, arena, content.ToolName,
                                           &arguments),
          "before-tool hook failed");
      if (!SessionResult_IsOk(hooked)) {
        beforeToolFailure = hooked;
      } else if (arguments.Length == 0) {
        beforeToolFailure = SessionResult_Error(
            SessionStatus::HookError, SVL("before-tool hook produced empty arguments"));
      } else {
        content.ToolArgsJson = StrView_Dup(arena, arguments);
      }
    }
    Array_Push(&effectiveAssistant.Content, content);
  }

  AgentToolBatchPlan plan = {};
  AgentToolBatch_Prepare(arena, harness->RuntimeTools, harness->Config.ToolCount,
                        &effectiveAssistant, harness->Config.ToolExec, &harness->AbortFlag,
                        &harness->Config.ToolHooks, &plan);
  if (!SessionResult_IsOk(beforeToolFailure)) {
    for (u32 i = 0; i < plan.Calls.Count; i++) {
      plan.Calls.Data[i].Runnable = false;
      plan.Calls.Data[i].Result = Array_Make<Content>(arena, 1);
      Array_Push(&plan.Calls.Data[i].Result, Content_Text(beforeToolFailure.Message));
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

  for (u32 i = 0; i < plan.Calls.Count; i++) {
    const Content* call = plan.Calls.Data[i].Call;
    AgentHarness_PublishToolStart(harness, call->ToolId, call->ToolName,
                                  plan.Calls.Data[i].EffectiveArgumentsJson);
  }

  AgentToolBatchResult results = {};
  AgentToolDispatchGate dispatch = {};
  dispatch.User = harness;
  dispatch.TryStart = TryStartTool;
  AgentToolEventSink events = ToolEvents(harness);
  AgentToolBatch_Run(arena, &plan, harness->Config.MaxParallelTools,
                     &harness->Config.ToolHooks, &dispatch, &events, &results);

  Array<SessionRecord> messages =
      Array_Make<SessionRecord>(arena, results.Calls.Count ? results.Calls.Count : 1);
  DurableId head = *ioHead;
  for (u32 i = 0; i < results.Calls.Count; i++) {
    SessionRecord message =
        NewMessageRecord(head, AgentToolResult_Materialize(&results.Calls.Data[i]));
    Array_Push(&messages, message);
    head = message.EntryId;
  }
  SessionResult committed = AgentHarness_Append(harness, messages.Data, messages.Count);
  if (SessionResult_IsOk(committed)) {
    *ioHead = head;
    *outAnyTool = results.AnyTool;
    *outAllTerminate = results.AllTerminate;
  }
  if (!SessionResult_IsOk(committed)) return committed;
  if (!SessionResult_IsOk(beforeToolFailure)) return beforeToolFailure;
  return harness->AbortFlag
             ? SessionResult_Error(SessionStatus::Cancelled,
                                   SVL("run cancelled during tool execution"))
             : committed;
}

StrView ExtractSummaryText(Arena* arena, const Message* assistant) {
  StrBuf text = StrBuf_Make(arena, 256);
  for (u32 i = 0; i < assistant->Content.Count; i++) {
    const Content* content = &assistant->Content.Data[i];
    if ((content->Kind == ContentKind::Text || content->Kind == ContentKind::Thinking) &&
        content->Text.Length) {
      if (text.Length) StrBuf_AppendChar(&text, '\n');
      StrBuf_Append(&text, content->Text);
    }
  }
  return StrBuf_View(&text);
}

struct CompactionTurnResult {
  AgentTurnOutcome Outcome;
  StrView Error;
  DurableId NewHead;
};

// Runs one compaction turn over the inclusive branch span [base..through] and
// appends the resulting Compaction entry chained from `head`. Publishes only
// turn-level events, so an inline compaction never emits a nested agent
// lifecycle inside the enclosing run.
SessionResult PerformCompaction(AgentHarness* harness, DurableId head,
                                DurableId baseEntryId, DurableId throughEntryId,
                                CompactionTurnResult* out) {
  AgentHarness_PublishTurnStart(harness, AgentTurnKind::Compaction);

  Message summarizePrompt = Message_User(
      &harness->Work,
      SVL("Summarize the conversation for future continuation. Preserve goals, "
          "decisions, constraints, unresolved work, and important tool results."));
  TurnRunResult turn = {};
  SessionResult result =
      RunAssistantTurn(harness, AgentTurnKind::Compaction, head, &summarizePrompt, &turn);

  AgentTurnOutcome outcome = AgentTurnOutcome::Succeeded;
  StrView error = {};

  if (SessionResult_IsOk(result)) {
    bool success = turn.ProviderOk && turn.Assistant.Stop != StopReason::Error &&
                   turn.Assistant.Stop != StopReason::Aborted;
    StrView summaryText =
        success ? ExtractSummaryText(&harness->Work, &turn.Assistant) : StrView{};
    if (!success || summaryText.Length == 0) {
      bool aborted = turn.Assistant.Stop == StopReason::Aborted;
      error = summaryText.Length == 0 && success
                  ? SVL("compaction provider returned an empty summary")
                  : turn.Assistant.ErrorMessage;
      result = SessionResult_Error(
          aborted ? SessionStatus::Cancelled : SessionStatus::ProviderError, error);
    } else {
      StrBuf wrapped = StrBuf_Make(&harness->Work, summaryText.Length + 48);
      StrBuf_AppendCStr(&wrapped, "Summary of previous conversation:\n");
      StrBuf_Append(&wrapped, summaryText);
      Message summary = Message_User(&harness->Work, StrBuf_View(&wrapped));

      SessionRecord compactionRecord = {};
      compactionRecord.EntryId = DurableId_Generate();
      compactionRecord.Kind = SessionRecordKind::Compaction;
      compactionRecord.CompactionEntry.ParentId = head;
      compactionRecord.CompactionEntry.BaseEntryId = baseEntryId;
      compactionRecord.CompactionEntry.ThroughEntryId = throughEntryId;
      compactionRecord.CompactionEntry.SourceHeadId = head;
      compactionRecord.CompactionEntry.Summary = summary;
      compactionRecord.CompactionEntry.HasUsage = true;
      compactionRecord.CompactionEntry.Use = turn.Assistant.Use;
      result = AgentHarness_Append(harness, &compactionRecord, 1, true);
      if (SessionResult_IsOk(result)) out->NewHead = compactionRecord.EntryId;
    }
  }

  if (!SessionResult_IsOk(result)) {
    outcome = result.Status == SessionStatus::Cancelled ? AgentTurnOutcome::Aborted
                                                        : AgentTurnOutcome::Failed;
    if (!error.Length) error = result.Message;
  }
  AgentHarness_PublishTurnEnd(harness, AgentTurnKind::Compaction, outcome, error);
  out->Outcome = outcome;
  out->Error = error;
  return result;
}

// The window the next provider request will actually be sized against: the
// explicitly selected tier when the model exposes one, else its fixed window.
u32 ActiveContextWindow(const AgentHarness* harness) {
  u32 window = harness->Config.Stream.Profile.ContextWindow;
  return window ? window : harness->Config.ModelInfo.ContextWindow;
}

// Decides whether the next provider request for `head` would eat into the
// window's reserved headroom, and if so which branch span to fold away.
// Measurement materializes the branch context once, the same O(branch) cost the
// turn itself pays.
bool ShouldAutoCompact(AgentHarness* harness, DurableId head, AgentCompactionCut* outCut) {
  const AgentCompactionPolicy* policy = &harness->Config.Compaction;
  u32 window = ActiveContextWindow(harness);
  if (!AgentCompactionPolicy_IsValid(policy) || !window) return false;

  SessionReduction reduction = AgentHarness_ReduceSnapshot(harness);
  Array<Message> messages = {};
  if (!SessionResult_IsOk(
          SessionReducer_Context(&harness->Work, &reduction, head, &messages)))
    return false;
  if (!AgentCompaction_ShouldCompact(
          policy, window, AgentCompaction_ContextTokens(messages.Data, messages.Count)))
    return false;

  Array<const SessionRecord*> branch = {};
  if (!SessionResult_IsOk(
          SessionReducer_Ancestry(&reduction, &harness->Work, head, &branch)))
    return false;
  return AgentCompaction_SelectCutPoint(branch.Data, branch.Count,
                                        policy->KeepRecentTokens, outCut);
}

SessionResult ContinueRun(AgentHarness* harness, DurableId* ioHead) {
  for (;;) {
    if (harness->AbortFlag)
      return SessionResult_Error(SessionStatus::Cancelled, SVL("run cancelled"));

    // Automatic compaction runs before the next provider request, at most once
    // per iteration. If it succeeds but the context still exceeds the budget we
    // continue anyway and let the provider decide, rather than loop.
    AgentCompactionCut cut = {};
    if (ShouldAutoCompact(harness, *ioHead, &cut)) {
      if (harness->Config.Hooks.BeforeCompaction) {
        SessionResult hooked = AgentHook_Failure(
            harness->Config.Hooks.BeforeCompaction(harness->Config.Hooks.User,
                                                   &harness->Work, *ioHead),
            "before-compaction hook failed");
        if (!SessionResult_IsOk(hooked)) return hooked;
      }
      CompactionTurnResult compaction = {};
      SessionResult compacted = PerformCompaction(harness, *ioHead, cut.BaseEntryId,
                                                  cut.ThroughEntryId, &compaction);
      if (!SessionResult_IsOk(compacted)) {
        if (compacted.Status == SessionStatus::Cancelled) return compacted;
        StrBuf message = StrBuf_Make(&harness->Work, compacted.Message.Length + 40);
        StrBuf_AppendCStr(&message, "automatic compaction failed");
        if (compacted.Message.Length) {
          StrBuf_AppendCStr(&message, ": ");
          StrBuf_Append(&message, compacted.Message);
        }
        return SessionResult_Error(compacted.Status, StrBuf_View(&message));
      }
      *ioHead = compaction.NewHead;
    }

    AgentHarness_PublishTurnStart(harness, AgentTurnKind::Run);
    TurnRunResult turn = {};
    SessionResult generated = RunAssistantTurn(harness, AgentTurnKind::Run, *ioHead, nullptr, &turn);
    if (!SessionResult_IsOk(generated)) {
      bool cancelled = generated.Status == SessionStatus::Cancelled;
      AgentHarness_PublishTurnEnd(harness, AgentTurnKind::Run,
                                 cancelled ? AgentTurnOutcome::Aborted : AgentTurnOutcome::Failed,
                                 generated.Message);
      return generated;
    }

    // message_end: append exactly one assistant message, regardless of stop
    // reason (Stop/ToolUse/Error/Aborted all become a durable message; only
    // the *next* provider request is what an error/abort short-circuits).
    SessionRecord assistantRecord = NewMessageRecord(*ioHead, turn.Assistant);
    SessionResult committed = AgentHarness_Append(harness, &assistantRecord, 1);
    if (!SessionResult_IsOk(committed)) {
      AgentHarness_PublishTurnEnd(harness, AgentTurnKind::Run, AgentTurnOutcome::Failed,
                                 committed.Message);
      return committed;
    }
    *ioHead = assistantRecord.EntryId;

    if (!turn.ProviderOk || turn.Assistant.Stop == StopReason::Error ||
        turn.Assistant.Stop == StopReason::Aborted) {
      bool aborted = turn.Assistant.Stop == StopReason::Aborted;
      AgentHarness_PublishTurnEnd(harness, AgentTurnKind::Run,
                                 aborted ? AgentTurnOutcome::Aborted : AgentTurnOutcome::Failed,
                                 turn.Assistant.ErrorMessage);
      return SessionResult_Error(aborted ? SessionStatus::Cancelled : SessionStatus::ProviderError,
                                 turn.Assistant.ErrorMessage);
    }

    bool anyTool = false;
    bool allTerminate = true;
    SessionResult tools = RunToolBatch(harness, ioHead, &turn.Assistant, &anyTool, &allTerminate);
    if (!SessionResult_IsOk(tools)) {
      AgentHarness_PublishTurnEnd(harness, AgentTurnKind::Run,
                                 tools.Status == SessionStatus::Cancelled
                                     ? AgentTurnOutcome::Aborted
                                     : AgentTurnOutcome::Failed,
                                 tools.Message);
      return tools;
    }
    AgentHarness_PublishTurnEnd(harness, AgentTurnKind::Run, AgentTurnOutcome::Succeeded, StrView{});

    // Steering folds onto the tree right after this tool-result batch, before
    // any further provider request (whether that request continues this
    // turn or starts the next one).
    u32 steerCount = 0;
    SessionResult steered =
        AgentHarness_FlushQueue(harness, &harness->Queues.Steering, ioHead, &steerCount);
    if (!SessionResult_IsOk(steered)) return steered;
    if (AgentCheckpoint_ContinueTurn(anyTool, allTerminate, steerCount)) continue;

    u32 followUpCount = 0;
    SessionResult followed =
        AgentHarness_FlushQueue(harness, &harness->Queues.FollowUp, ioHead, &followUpCount);
    if (!SessionResult_IsOk(followed)) return followed;
    if (AgentCheckpoint_ContinueRun(followUpCount)) continue;

    return SessionResult_Ok();
  }
}

}  // namespace

SessionResult AgentDriver_Run(AgentHarness* harness, StrView prompt) {
  if (prompt.Length == 0)
    return SessionResult_Error(SessionStatus::InvalidArgument,
                               SVL("run prompt must not be empty"));
  AgentHarness_PublishAgentStart(harness, AgentTurnKind::Run);

  DurableId head = AgentHarness_ReduceSnapshot(harness).HeadId;
  u32 nextTurnCount = 0;
  SessionResult result =
      AgentHarness_FlushQueue(harness, &harness->Queues.NextTurn, &head, &nextTurnCount);

  if (SessionResult_IsOk(result)) {
    Message accepted = Message_User(&harness->Work, StrView_Dup(&harness->Work, prompt));
    if (harness->Config.Hooks.BeforeRun) {
      result = AgentHook_Failure(
          harness->Config.Hooks.BeforeRun(harness->Config.Hooks.User, &harness->Work, &accepted),
          "before-run hook failed");
    }
    if (SessionResult_IsOk(result)) {
      SessionRecord userRecord = NewMessageRecord(head, accepted);
      result = AgentHarness_Append(harness, &userRecord, 1);
      if (SessionResult_IsOk(result)) head = userRecord.EntryId;
    }
  }

  if (SessionResult_IsOk(result)) {
    u32 steerCount = 0;
    result = AgentHarness_FlushQueue(harness, &harness->Queues.Steering, &head, &steerCount);
  }

  if (SessionResult_IsOk(result)) result = ContinueRun(harness, &head);

  AgentTurnOutcome outcome = AgentTurnOutcome::Succeeded;
  StrView error = {};
  if (!SessionResult_IsOk(result)) {
    outcome =
        result.Status == SessionStatus::Cancelled ? AgentTurnOutcome::Aborted : AgentTurnOutcome::Failed;
    error = result.Message;
  }
  if (harness->Config.Hooks.AfterTurn) {
    SessionResult afterTurn = AgentHook_Failure(
        harness->Config.Hooks.AfterTurn(harness->Config.Hooks.User, &harness->Work,
                                        AgentTurnKind::Run, outcome, error),
        "after-turn hook failed");
    if (SessionResult_IsOk(result) && !SessionResult_IsOk(afterTurn)) {
      result = afterTurn;
      outcome = AgentTurnOutcome::Failed;
      error = result.Message;
    }
  }
  return result;
}

SessionResult AgentDriver_Compact(AgentHarness* harness) {
  AgentHarness_PublishAgentStart(harness, AgentTurnKind::Compaction);
  SessionReduction initial = AgentHarness_ReduceSnapshot(harness);
  DurableId head = initial.HeadId;

  SessionResult result = SessionResult_Ok();
  if (harness->Config.Hooks.BeforeCompaction) {
    result = AgentHook_Failure(
        harness->Config.Hooks.BeforeCompaction(harness->Config.Hooks.User, &harness->Work, head),
        "before-compaction hook failed");
  }

  AgentTurnOutcome outcome = AgentTurnOutcome::Failed;
  StrView error = {};

  if (SessionResult_IsOk(result)) {
    // Manual compaction always folds the whole branch: base is the session
    // root, which is a tree node on every branch.
    CompactionTurnResult compaction = {};
    result = PerformCompaction(
        harness, head, initial.RecordCount ? initial.Records[0].EntryId : DurableId{},
        head, &compaction);
    outcome = compaction.Outcome;
    error = compaction.Error;
  } else {
    error = result.Message;
  }

  if (harness->Config.Hooks.AfterTurn) {
    SessionResult afterTurn = AgentHook_Failure(
        harness->Config.Hooks.AfterTurn(harness->Config.Hooks.User, &harness->Work,
                                        AgentTurnKind::Compaction, outcome, error),
        "after-turn hook failed");
    if (SessionResult_IsOk(result) && !SessionResult_IsOk(afterTurn)) {
      result = afterTurn;
      outcome = AgentTurnOutcome::Failed;
      error = result.Message;
    }
  }
  return result;
}
