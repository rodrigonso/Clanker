#include "app/event_pump.h"

#include <stdio.h>
#if !defined(_WIN32)
#include <signal.h>
#endif

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
    case AgentHarnessEventKind::TransientPermissionReviewStart:
      fprintf(stderr, "\nreviewing %.*s permission with %.*s\n",
              (int)ev->PermissionReview.ToolName.Length,
              ev->PermissionReview.ToolName.Data,
              (int)ev->PermissionReview.Model.Length,
              ev->PermissionReview.Model.Data);
      break;
    default:
      break;
  }
}

void ReportHarnessResult(SessionResult result) {
  if (SessionResult_IsOk(result)) {
    // An Ok result normally carries no message. When it does, the run ended
    // early for a reason the user needs -- the turn limit is the only source
    // today -- and nothing else surfaces it: agent_end's message is not
    // rendered by either REPL.
    if (result.Message.Length)
      printf("\n%.*s\n", (int)result.Message.Length, result.Message.Data);
    return;
  }
  fprintf(stderr, "\nerror: %.*s\n", (int)result.Message.Length,
          result.Message.Data);
}

namespace {

static bool WriteJsonFile(void* user, StrView line) {
  FILE* file = (FILE*)user;
  return fwrite(line.Data, 1, line.Length, file) == line.Length &&
         fflush(file) == 0;
}

static void FailJsonOutput(HarnessJsonOutput* output, SessionResult failure) {
  output->Failure = failure;
  Atomic_StoreI32(&output->Harness->AbortFlag, 1);
}

static void WriteJsonLine(HarnessJsonOutput* output, StrBuf* line) {
  StrBuf_AppendChar(line, '\n');
  if (!output->Writer.Write(output->Writer.User, StrBuf_View(line)))
    FailJsonOutput(output, SessionResult_Error(
                              SessionStatus::IoError,
                              SVL("failed to write or flush JSON output")));
}

static void ObserveJsonEvent(void* user, const AgentHarnessEvent* event) {
  HarnessJsonOutput* output = (HarnessJsonOutput*)user;
  AppExtensions_OnEvent(output->Extensions, event);
  if (!SessionResult_IsOk(output->Failure)) return;
  ArenaReset(&output->Scratch);
  ArenaTrim(&output->Scratch, 64 * 1024);
  StrBuf line = StrBuf_Make(&output->Scratch, 256);
  SessionResult encoded = AgentEventJson_Write(&line, event);
  if (!SessionResult_IsOk(encoded)) {
    FailJsonOutput(output, SessionResult_Error(
                              encoded.Status, SVL("failed to encode JSON event")));
    return;
  }
  if (line.Length) WriteJsonLine(output, &line);
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
  replay.Tool.Name = message->ToolName;
  replay.Tool.Result.Content = message->Content;
  replay.Tool.IsError = message->IsError;
  HarnessEventSink_Emit(pump->Sink, &replay);
}

// Committed pages the pump keeps between events. One event is normally a few
// KB; a tool result or a post-lag snapshot can spike far higher, and those
// pages should not stay resident for the rest of the session.
constexpr usize PUMP_SCRATCH_KEEP_BYTES = (usize)1 << 20;

static SessionResult RegisterHarnessWatch(HarnessEventPump* pump) {
  if (!pump->TuiView)
    return AgentHarness_Watch(pump->Harness, AGENT_HARNESS_MAX_WATCH_EVENTS,
                              &pump->Watch);

  // Registration and history share one cursor. Hydrate before consuming any
  // queued events, then release the copy rather than retaining it in event
  // scratch or the runtime arena.
  Arena snapshotMem = ArenaCreate();
  AgentTranscriptSnapshot snapshot = {};
  SessionResult watched = AgentHarness_WatchTranscript(
      &snapshotMem, pump->Harness, AGENT_HARNESS_MAX_WATCH_EVENTS, 0,
      &snapshot, &pump->Watch);
  if (SessionResult_IsOk(watched)) {
    Tui_ReplaceTranscript(pump->TuiView, snapshot.Transcript.Data,
                          snapshot.Transcript.Count);
    Tui_SetContextTokens(pump->TuiView, snapshot.ContextTokens);
  }
  ArenaRelease(&snapshotMem);
  return watched;
}

static void RunHarnessEventPump(void* user) {
  HarnessEventPump* pump = (HarnessEventPump*)user;
  Arena scratch = ArenaCreate();
  for (;;) {
    ArenaReset(&scratch);
    ArenaTrim(&scratch, PUMP_SCRATCH_KEEP_BYTES);
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
      SessionResult watched = RegisterHarnessWatch(pump);
      ClearLiveToolEnds(pump);
      Mutex_Unlock(&pump->WatchGuard);
      if (!SessionResult_IsOk(watched)) break;
      continue;
    }
    if (!SessionResult_IsOk(next)) break;
    AppExtensions_OnEvent(pump->Extensions, &event);
    PumpHarnessEvent(pump, &event);
  }
  ArenaRelease(&scratch);
}

}  // namespace

SessionResult StartHarnessEventPump(AgentHarness* harness,
                                    HarnessEventSink sink, Tui* tuiView,
                                    HarnessEventPump* pump,
                                    AppExtensions* extensions) {
  memset(pump, 0, sizeof(*pump));
  pump->Harness = harness;
  pump->TuiView = tuiView;
  pump->Sink = sink;
  pump->Extensions = extensions;
  pump->CompletionMem = ArenaCreate((usize)4 << 20);
  ClearLiveToolEnds(pump);
  Mutex_Init(&pump->WatchGuard);
  pump->WatchGuardInitialized = true;
  SessionResult watched = RegisterHarnessWatch(pump);
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

SessionResult StartHarnessJsonOutput(AgentHarness* harness,
                                     HarnessJsonOutput* output,
                                     HarnessJsonWriter writer,
                                     AppExtensions* extensions) {
  if (!harness || !harness->Initialized || !output)
    return SessionResult_Invalid("JSON output arguments are invalid");
  ScopedLock state(&harness->StateLock);
  ScopedLock events(&harness->WatcherLock);
  if (harness->Running || harness->Config.Events.OnEvent || output->Started)
    return SessionResult_Error(SessionStatus::Busy,
                               SVL("JSON output requires an idle, unobserved harness"));
#if !defined(_WIN32)
  // Process-wide, like the shell's stdin writer: a closed consumer must return
  // EPIPE rather than terminating before we can report the delivery failure.
  if (!writer.Write && signal(SIGPIPE, SIG_IGN) == SIG_ERR)
    return SessionResult_Error(SessionStatus::IoError,
                               SVL("failed to configure JSON pipe error handling"));
#endif
  *output = {};
  output->Harness = harness;
  output->Extensions = extensions;
  output->Scratch = ArenaCreate();
  output->Writer = writer.Write ? writer : HarnessJsonWriter{stdout, WriteJsonFile};
  output->Failure = SessionResult_Ok();
  output->Started = true;
  harness->Config.Events = {output, ObserveJsonEvent};
  return SessionResult_Ok();
}

SessionResult FinishHarnessJsonOutput(HarnessJsonOutput* output,
                                      SessionResult result) {
  if (!output || !output->Started)
    return SessionResult_Invalid("JSON output is not started");
  {
    ScopedLock state(&output->Harness->StateLock);
    ScopedLock events(&output->Harness->WatcherLock);
    if (output->Harness->Running)
      return SessionResult_Error(SessionStatus::Busy,
                                 SVL("cannot finish JSON output during a run"));
    output->Harness->Config.Events = {};
  }
  if (SessionResult_IsOk(output->Failure)) {
    ArenaReset(&output->Scratch);
    StrBuf line = StrBuf_Make(&output->Scratch, 128);
    AgentEventJson_WriteResult(&line, result);
    WriteJsonLine(output, &line);
  }
  if (!SessionResult_IsOk(output->Failure)) result = output->Failure;
  ArenaRelease(&output->Scratch);
  output->Started = false;
  output->Harness = nullptr;
  return result;
}
