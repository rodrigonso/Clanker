#include "app/event_pump.h"

#include <stdio.h>

#include "agent/event_json.h"
#include "term/sanitize.h"

namespace {

void PrintPreview(StrView t) {
  // Reused scratch (REPL events are emitted on a single thread).
  static Arena sa = ArenaCreate((usize)16 << 20);
  static StrBuf sb = StrBuf_Make(&sa, 256);
  StrBuf_Clear(&sb);
  Sanitize_Display(&sb, t);  // strip \r / ANSI / control bytes from tool output
  StrView s = StrBuf_View(&sb);
  u32 n = s.Length > 200 ? 200 : s.Length;
  fputs("\x1b[90m", stdout);
  for (u32 i = 0; i < n; i++) {
    char c = s.Data[i];
    putchar(c == '\n' ? ' ' : c);
  }
  if (s.Length > 200) fputs("\xe2\x80\xa6", stdout);
  fputs("\x1b[0m\n", stdout);
}

}  // namespace

void ReplSink(void* user, const AgentHarnessEvent* ev) {
  (void)user;
  switch (ev->Kind) {
    case AgentHarnessEventKind::TransientStream:
      if (ev->Stream.Kind == StreamEventKind::TextDelta) {
        fwrite(ev->Stream.Delta.Data, 1, ev->Stream.Delta.Length, stdout);
        fflush(stdout);
      }
      break;
    case AgentHarnessEventKind::TransientToolStart:
      printf("\n\x1b[36m\xe2\x9a\x99 %.*s\x1b[0m \x1b[90m%.*s\x1b[0m\n",
             (int)ev->Tool.Name.Length, ev->Tool.Name.Data,
             (int)ev->Tool.ArgumentsJson.Length, ev->Tool.ArgumentsJson.Data);
      break;
    case AgentHarnessEventKind::TransientToolEnd:
      if (ev->Tool.Result.Content.Count > 0)
        PrintPreview(ev->Tool.Result.Content.Data[0].Text);
      break;
    case AgentHarnessEventKind::DurableRecord:
      if (ev->Record->Kind == SessionRecordKind::Message &&
          ev->Record->MessageEntry.Value.Kind == Role::Assistant)
        putchar('\n');
      break;
    case AgentHarnessEventKind::TransientRetryStart:
      fprintf(stderr, "\nretrying %s in %u ms (retry %u of %u): %.*s\n",
              ev->Retry.Layer == RetryLayer::ProviderRequest
                  ? "provider request"
                  : "assistant turn",
              ev->Retry.DelayMs, ev->Retry.Attempt, ev->Retry.MaxAttempts,
              (int)ev->Retry.Error.Length, ev->Retry.Error.Data);
      break;
    default:
      break;
  }
}

void ReportHarnessResult(SessionResult result) {
  if (SessionResult_IsOk(result)) return;
  fprintf(stderr, "\nerror: %.*s\n", (int)result.Message.Length,
          result.Message.Data);
}

namespace {

// One event, one line, flushed immediately so a consumer reading the pipe sees
// progress as it happens. Events with no JSON representation emit nothing.
static void EmitHarnessEventJson(Arena* arena, const AgentHarnessEvent* event) {
  StrBuf line = StrBuf_Make(arena, 256);
  if (!AgentEventJson_Write(&line, event)) return;
  fwrite(line.Data, 1, line.Length, stdout);
  fputc('\n', stdout);
  fflush(stdout);
}

static void ClearLiveToolEnds(HarnessEventPump* pump) {
  ArenaReset(&pump->CompletionMem);
  pump->LiveToolEnds = Array_Make<StrView>(&pump->CompletionMem, 16);
}

static void RememberLiveToolEnd(HarnessEventPump* pump, StrView toolCallId) {
  Array_Push(&pump->LiveToolEnds,
             StrView_Dup(&pump->CompletionMem, toolCallId));
}

static bool ConsumeLiveToolEnd(HarnessEventPump* pump, StrView toolCallId) {
  for (u32 i = 0; i < pump->LiveToolEnds.Count; i++) {
    if (!StrView_Eq(pump->LiveToolEnds.Data[i], toolCallId)) continue;
    for (u32 j = i + 1; j < pump->LiveToolEnds.Count; j++)
      pump->LiveToolEnds.Data[j - 1] = pump->LiveToolEnds.Data[j];
    pump->LiveToolEnds.Count--;
    if (pump->LiveToolEnds.Count == 0) ClearLiveToolEnds(pump);
    return true;
  }
  return false;
}

// Forwards the watch stream to the active consumer, collapsing the two sources
// of a tool completion into one. A tool's result surfaces twice -- once as the
// live TransientToolEnd and again as the durable ToolResult record -- so
// whichever arrives first wins and the other is suppressed.
static void PumpHarnessEvent(HarnessEventPump* pump,
                             const AgentHarnessEvent* event) {
  if (event->Kind == AgentHarnessEventKind::TransientToolEnd) {
    HarnessEventSink_Emit(pump->Sink, event);
    RememberLiveToolEnd(pump, event->Tool.CallId);
    return;
  }
  if (event->Kind == AgentHarnessEventKind::TransientAgentEnd) {
    HarnessEventSink_Emit(pump->Sink, event);
    ClearLiveToolEnds(pump);
    return;
  }
  if (event->Kind != AgentHarnessEventKind::DurableRecord) {
    HarnessEventSink_Emit(pump->Sink, event);
    return;
  }

  const SessionRecord* record = event->Record;
  if (record->Kind != SessionRecordKind::Message) return;
  const Message* message = &record->MessageEntry.Value;
  if (message->Kind == Role::Assistant) {
    HarnessEventSink_Emit(pump->Sink, event);
    return;
  }
  if (message->Kind != Role::ToolResult) return;
  if (ConsumeLiveToolEnd(pump, message->ToolCallId)) return;
  // No live end was seen for this call, so replay the durable result in the
  // same shape a live completion would have taken.
  AgentHarnessEvent replay = {};
  replay.Kind = AgentHarnessEventKind::TransientToolEnd;
  replay.Cursor = event->Cursor;
  replay.Tool.CallId = message->ToolCallId;
  replay.Tool.Result.Content = message->Content;
  replay.Tool.IsError = message->IsError;
  HarnessEventSink_Emit(pump->Sink, &replay);
}

static void RunHarnessEventPump(void* user) {
  HarnessEventPump* pump = (HarnessEventPump*)user;
  Arena scratch = ArenaCreate((usize)16 << 20);
  for (;;) {
    ArenaReset(&scratch);
    AgentHarnessEvent event = {};
    SessionResult next =
        AgentHarness_WatchNext(&scratch, &pump->Watch, &event);
    if (next.Status == SessionStatus::WatcherLagged) {
      Mutex_Lock(&pump->WatchGuard);
      if (pump->StopRequested) {
        Mutex_Unlock(&pump->WatchGuard);
        break;
      }
      AgentHarness_WatchClose(&pump->Watch);
      AgentHarnessSnapshot snapshot = {};
      SessionResult watched =
          AgentHarness_Watch(&scratch, pump->Harness,
                             AGENT_HARNESS_MAX_WATCH_EVENTS, &snapshot,
                             &pump->Watch);
      ClearLiveToolEnds(pump);
      if (SessionResult_IsOk(watched) && pump->TuiView)
        Tui_ReplaceTranscript(pump->TuiView, snapshot.Transcript.Data,
                              snapshot.Transcript.Count);
      Mutex_Unlock(&pump->WatchGuard);
      if (!SessionResult_IsOk(watched)) break;
      continue;
    }
    if (!SessionResult_IsOk(next)) break;
    // Every run-scoped hook event is dispatched from here rather than from
    // around AgentHarness_Run, because this watch stream is the only place the
    // agent's events are already in order. Reporting Stop from the calling
    // thread raced the pump and could land it ahead of the run's own last
    // PostToolUse. Reported for every output mode: hooks describe the agent's
    // lifecycle, not whatever this process happens to be rendering.
    switch (event.Kind) {
      // PreToolUse is absent deliberately: it is evaluated synchronously by the
      // gate during clearance, strictly earlier than this, because its verdict
      // decides whether the tool runs at all.
      case AgentHarnessEventKind::TransientToolEnd:
        HookRunner_Dispatch(g_hooks, HookEvent::PostToolUse, event.Tool.Name);
        break;
      case AgentHarnessEventKind::TransientAgentStart:
        // Compaction is machinery, not a prompt the user submitted.
        if (event.Turn.Kind == AgentTurnKind::Run)
          HookRunner_Dispatch(g_hooks, HookEvent::UserPromptSubmit, StrView{});
        break;
      case AgentHarnessEventKind::TransientAgentEnd:
        // A cancel is how a user stops a run they no longer want, so it ends
        // the turn normally; only an actual error is reported as a failure.
        if (event.Turn.Kind == AgentTurnKind::Run)
          HookRunner_Dispatch(g_hooks,
                              event.Turn.Outcome == AgentTurnOutcome::Failed
                                  ? HookEvent::StopFailure
                                  : HookEvent::Stop,
                              StrView{});
        break;
      default:
        break;
    }

    if (pump->JsonOutput)
      EmitHarnessEventJson(&scratch, &event);
    else
      PumpHarnessEvent(pump, &event);
  }
  ArenaRelease(&scratch);
  pump->Exited = 1;
}

}  // namespace

SessionResult StartHarnessEventPump(Arena* arena, AgentHarness* harness,
                                    HarnessEventSink sink, Tui* tuiView,
                                    HarnessEventPump* pump,
                                    AgentHarnessSnapshot* snapshot,
                                    bool jsonOutput) {
  memset(pump, 0, sizeof(*pump));
  pump->Harness = harness;
  pump->TuiView = tuiView;
  pump->Sink = sink;
  pump->JsonOutput = jsonOutput;
  pump->CompletionMem = ArenaCreate((usize)4 << 20);
  ClearLiveToolEnds(pump);
  Mutex_Init(&pump->WatchGuard);
  pump->WatchGuardInitialized = true;
  SessionResult watched = AgentHarness_Watch(
      arena, harness, AGENT_HARNESS_MAX_WATCH_EVENTS, snapshot,
      &pump->Watch);
  if (SessionResult_IsOk(watched)) {
    if (!Thread_Start(&pump->Worker, RunHarnessEventPump, pump)) {
      AgentHarness_WatchClose(&pump->Watch);
      Mutex_Destroy(&pump->WatchGuard);
      ArenaRelease(&pump->CompletionMem);
      pump->WatchGuardInitialized = false;
      watched = SessionResult_Error(SessionStatus::IoError,
                                    SVL("failed to start session event pump"));
    } else {
      pump->Started = true;
    }
  }
  if (!SessionResult_IsOk(watched) && pump->WatchGuardInitialized) {
    Mutex_Destroy(&pump->WatchGuard);
    ArenaRelease(&pump->CompletionMem);
    pump->WatchGuardInitialized = false;
  }
  return watched;
}

void StopHarnessEventPump(HarnessEventPump* pump) {
  if (!pump || !pump->Started) return;
  Mutex_Lock(&pump->WatchGuard);
  pump->StopRequested = true;
  AgentHarness_WatchRequestClose(&pump->Watch);
  Mutex_Unlock(&pump->WatchGuard);
  Thread_Join(&pump->Worker);
  Mutex_Lock(&pump->WatchGuard);
  if (pump->Watch.Initialized) AgentHarness_WatchClose(&pump->Watch);
  Mutex_Unlock(&pump->WatchGuard);
  Mutex_Destroy(&pump->WatchGuard);
  ArenaRelease(&pump->CompletionMem);
  pump->WatchGuardInitialized = false;
  pump->Started = false;
}
