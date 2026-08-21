#include "app/tui_bridge.h"

namespace {

// The first content block is what the transcript shows as a tool's preview.
StrView FirstText(const ToolResult& result) {
  if (result.Content.Count == 0) return StrView{};
  return result.Content.Data[0].Text;
}

bool BridgeSteer(void* user, StrView text, StrView* outError) {
  AgentHarness* harness = (AgentHarness*)user;
  SessionResult queued = AgentHarness_QueueSteering(harness, text);
  if (SessionResult_IsOk(queued)) return true;
  if (outError) *outError = queued.Message;
  return false;
}

bool BridgeRequestCancel(void* user) {
  AgentHarness* harness = (AgentHarness*)user;
  return SessionResult_IsOk(AgentHarness_RequestCancel(harness));
}

// Deliberately an unsynchronized read, matching what the TUI did when it held
// the harness pointer directly. Taking StateLock here would be the first place
// anything acquires it beneath the TUI lock during a clear, and the answer is
// advisory either way -- the caller re-checks under its own lock.
bool BridgeIsBusy(void* user) {
  AgentHarness* harness = (AgentHarness*)user;
  return harness && harness->Running;
}

TuiPermissionMode BridgeCyclePermission(void* user) {
  return TuiBridge_ToTuiPermissionMode(
      AgentHarness_CyclePermissionMode((AgentHarness*)user));
}

}  // namespace

TuiPermissionMode TuiBridge_ToTuiPermissionMode(AgentPermissionMode mode) {
  switch (mode) {
    case AgentPermissionMode::Ask: return TuiPermissionMode::Ask;
    case AgentPermissionMode::Auto: return TuiPermissionMode::Auto;
    case AgentPermissionMode::Yolo: return TuiPermissionMode::Yolo;
  }
  return TuiPermissionMode::Auto;
}

AgentPermissionMode TuiBridge_FromTuiPermissionMode(TuiPermissionMode mode) {
  switch (mode) {
    case TuiPermissionMode::Ask: return AgentPermissionMode::Ask;
    case TuiPermissionMode::Auto: return AgentPermissionMode::Auto;
    case TuiPermissionMode::Yolo: return AgentPermissionMode::Yolo;
  }
  return AgentPermissionMode::Ask;
}

TuiAgentHooks TuiBridge_AgentHooks(AgentHarness* harness) {
  TuiAgentHooks hooks = {};
  hooks.User = harness;
  hooks.Steer = BridgeSteer;
  hooks.RequestCancel = BridgeRequestCancel;
  hooks.IsBusy = BridgeIsBusy;
  hooks.CyclePermission = BridgeCyclePermission;
  return hooks;
}

void TuiBridge_HarnessEvent(void* user, const AgentHarnessEvent* ev) {
  Tui* t = (Tui*)user;
  switch (ev->Kind) {
    case AgentHarnessEventKind::TransientAgentStart:
      Tui_TurnStarted(t);
      break;
    case AgentHarnessEventKind::TransientStream:
      if (ev->Stream.Kind == StreamEventKind::TextDelta)
        Tui_AppendAssistantText(t, ev->Stream.Delta);
      break;
    // The assistant turn has no transient "message end"; the durable record is
    // the authoritative completion, carrying the final stop reason and usage.
    case AgentHarnessEventKind::DurableRecord: {
      if (ev->Record->Kind != SessionRecordKind::Message) break;
      const Message* message = &ev->Record->MessageEntry.Value;
      if (message->Kind != Role::Assistant) break;
      Tui_AssistantMessageEnded(t, message->Stop, message->Use,
                                message->ErrorMessage);
      break;
    }
    case AgentHarnessEventKind::TransientToolStart:
      Tui_ToolStarted(t, ev->Tool.CallId, ev->Tool.Name, ev->Tool.ArgumentsJson);
      break;
    case AgentHarnessEventKind::TransientToolUpdate:
      Tui_ToolProgress(t, ev->Tool.CallId, FirstText(ev->Tool.Result));
      break;
    case AgentHarnessEventKind::TransientToolEnd:
      Tui_ToolEnded(t, ev->Tool.CallId, FirstText(ev->Tool.Result),
                    ev->Tool.IsError);
      break;
    case AgentHarnessEventKind::TransientAgentEnd:
      Tui_TurnEnded(t);
      break;
    case AgentHarnessEventKind::TransientRetryStart:
      Tui_RetryStarted(t, ev->Retry.Layer == RetryLayer::ProviderRequest,
                       ev->Retry.DelayMs, ev->Retry.Attempt,
                       ev->Retry.MaxAttempts);
      break;
    case AgentHarnessEventKind::TransientRetryEnd:
      Tui_RetryEnded(t);
      break;
    case AgentHarnessEventKind::TransientPermissionReviewStart:
      Tui_PermissionReviewStarted(t, ev->PermissionReview.ToolName);
      break;
    case AgentHarnessEventKind::TransientPermissionReviewEnd:
      Tui_PermissionReviewEnded(t);
      break;
    case AgentHarnessEventKind::TransientQueueUpdate:
      Tui_SetQueuedCount(t, ev->Queue.SteerCount + ev->Queue.FollowUpCount +
                                ev->Queue.NextTurnCount);
      break;
    case AgentHarnessEventKind::TransientEventsDropped:
      Tui_NoticeDroppedEvents(t, ev->Dropped.Count);
      break;
    default:
      break;
  }
}
