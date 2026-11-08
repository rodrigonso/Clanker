#include "app/commands.h"

#include "app/dream.h"
#include "app/memory.h"
#include "app/session_switch.h"
#include "app/wiki.h"
#include "tools/fs.h"

namespace {

bool CurrentWiki(InteractiveRuntime* rt, Arena* arena, WikiLocation* out) {
  StrView store = rt->Memory ? rt->Memory->Store : Wiki_DefaultStore(arena);
  return Wiki_Locate(arena, store, rt->WorkingDirectory, out);
}

AppCommandHandlerResult Failed(StrView message) {
  return AppCommandHandlerResult{AppCommandHandlerStatus::Failed, message};
}

AppCommandHandlerResult Succeeded(StrView message) {
  return AppCommandHandlerResult{AppCommandHandlerStatus::Success, message};
}

AppCommandHandlerResult SetMemory(InteractiveRuntime* rt, bool enabled) {
  if (!rt->Harness || rt->Harness->Running)
    return Failed(SVL("Memory can only be switched while the agent is idle."));
  bool previous = rt->MemoryEnabled;
  rt->MemoryEnabled = enabled;
  MemoryExtension_SetEnabled(rt->Memory, enabled);
  StrBuf error = StrBuf_Make(&rt->CommandMem, 160);
  if (!RuntimeRebuildPrompt(rt, &error)) {
    rt->MemoryEnabled = previous;
    MemoryExtension_SetEnabled(rt->Memory, previous);
    return Failed(StrBuf_View(&error));
  }
  StrBuf message = StrBuf_Make(&rt->CommandMem, 160);
  StrBuf_AppendCStr(&message, enabled ? "Memory wiki on." : "Memory wiki off.");
  if (!rt->Memory)
    StrBuf_AppendCStr(&message, " It stays off for this run (--no-memory or CLANKER_NO_MEMORY).");
  if (!RuntimePersistSettings(rt)) {
    StrBuf_AppendCStr(&message, " Settings could not be saved.");
    return Failed(StrBuf_View(&message));
  }
  return Succeeded(StrBuf_View(&message));
}

AppCommandHandlerResult Lint(InteractiveRuntime* rt, const WikiLocation* location) {
  Arena* arena = &rt->CommandMem;
  Wiki wiki = {};
  Wiki_Scan(arena, location->Pages, &wiki);
  Array<WikiIssue> issues = Array_Make<WikiIssue>(arena, 16);
  Wiki_Lint(arena, &wiki, &issues);
  if (!issues.Count) return Succeeded(SVL("The memory wiki has no lint findings."));
  StrBuf message = StrBuf_Make(arena, 1024);
  StrBuf_AppendCStr(&message, "Memory wiki lint:\n");
  Wiki_AppendIssues(&message, &issues);
  StrBuf_AppendCStr(&message, "Run /dream to have them fixed.");
  return Succeeded(StrBuf_View(&message));
}

AppCommandHandlerResult Restore(InteractiveRuntime* rt, const WikiLocation* location) {
  if (!rt->Harness || rt->Harness->Running)
    return Failed(SVL("The wiki can only be restored while the agent is idle."));
  u32 pages = 0;
  StrView snapshot = {};
  switch (Dream_RestoreLatestSnapshot(&rt->CommandMem, location, &pages, &snapshot)) {
    case DreamRestoreResult::NoSnapshot:
      return Failed(SVL("There is no snapshot to restore; one is taken before every dream."));
    case DreamRestoreResult::Busy:
      return Failed(SVL("A dream is running; restore once it finishes."));
    case DreamRestoreResult::Failed:
      return Failed(SVL("The snapshot could not be restored."));
    case DreamRestoreResult::Restored:
      break;
  }
  StrBuf error = StrBuf_Make(&rt->CommandMem, 160);
  bool rebuilt = RuntimeRebuildPrompt(rt, &error);
  StrBuf message = StrBuf_Make(&rt->CommandMem, 160);
  StrBuf_AppendCStr(&message, "Restored ");
  StrBuf_AppendU64(&message, pages);
  StrBuf_AppendCStr(&message, pages == 1 ? " page from snapshot " : " pages from snapshot ");
  StrBuf_Append(&message, snapshot);
  StrBuf_AppendChar(&message, '.');
  if (!rebuilt) StrBuf_AppendCStr(&message, " Run /reload to refresh the prompt.");
  return Succeeded(StrBuf_View(&message));
}

AppCommandHandlerResult Status(InteractiveRuntime* rt, const WikiLocation* location) {
  Arena* arena = &rt->CommandMem;
  Wiki wiki = {};
  Wiki_Scan(arena, location->Pages, &wiki);
  Array<WikiIssue> issues = Array_Make<WikiIssue>(arena, 16);
  Wiki_Lint(arena, &wiki, &issues);
  u32 errors = Wiki_ErrorCount(&issues);
  DreamStatus dream = {};
  Dream_ReadStatus(arena, location, &dream);

  StrBuf message = StrBuf_Make(arena, 1024);
  StrBuf_AppendCStr(&message, "Memory: ");
  StrBuf_AppendCStr(&message, !rt->MemoryEnabled ? "off"
                              : rt->Memory       ? "on"
                                                 : "off for this run");
  StrBuf_AppendCStr(&message, "\nRepository: ");
  StrBuf_Append(&message, location->Repository);
  StrBuf_AppendCStr(&message, "\nWiki: ");
  StrBuf_Append(&message, location->Pages);
  StrBuf_AppendCStr(&message, "\nPages: ");
  StrBuf_AppendU64(&message, wiki.Pages.Count);
  StrBuf_AppendCStr(&message, " (");
  StrBuf_AppendU64(&message, wiki.TotalBytes);
  StrBuf_AppendCStr(&message, " of ");
  StrBuf_AppendU64(&message, WIKI_TOTAL_MAX_BYTES);
  StrBuf_AppendCStr(&message, " bytes); pinned ");
  StrBuf_AppendU64(&message, wiki.PinnedPages);
  StrBuf_AppendCStr(&message, " (");
  StrBuf_AppendU64(&message, wiki.PinnedBytes);
  StrBuf_AppendCStr(&message, " of ");
  StrBuf_AppendU64(&message, WIKI_PINNED_MAX_BYTES);
  StrBuf_AppendCStr(&message, " bytes)\nLint: ");
  StrBuf_AppendU64(&message, errors);
  StrBuf_AppendCStr(&message, errors == 1 ? " error, " : " errors, ");
  StrBuf_AppendU64(&message, issues.Count - errors);
  StrBuf_AppendCStr(&message, issues.Count - errors == 1 ? " warning" : " warnings");
  if (issues.Count) StrBuf_AppendCStr(&message, " (/memory lint)");
  StrBuf_AppendCStr(&message, "\nDreaming: ");
  if (dream.Running) {
    StrBuf_AppendCStr(&message, "running now (/dream watch)");
  } else if (dream.HasLastDream) {
    StrBuf_AppendCStr(&message, "last dream ");
    StrBuf_Append(&message, FormatLocalTime(arena, dream.LastDream.TimeMs));
    StrBuf_AppendCStr(&message, ", ");
    StrBuf_Append(&message, dream.LastDream.Status.Length ? dream.LastDream.Status
                                                          : SVL("unknown"));
    StrBuf_AppendCStr(&message, ", ");
    StrBuf_AppendU64(&message, dream.LastDream.PagesChanged);
    StrBuf_AppendCStr(&message, dream.LastDream.PagesChanged == 1 ? " page changed"
                                                                  : " pages changed");
  } else {
    StrBuf_AppendCStr(&message, "never (/dream)");
  }
  StrBuf_AppendCStr(&message, "\nSessions since the last dream: ");
  StrBuf_AppendU64(&message, dream.PendingSessions);
  StrBuf_AppendCStr(&message, "\nSnapshots: ");
  StrBuf_AppendU64(&message, dream.Snapshots);
  if (dream.Snapshots) StrBuf_AppendCStr(&message, " (/memory restore)");
  return Succeeded(StrBuf_View(&message));
}

// Starts a background pass. Empty on success, otherwise why it could not.
StrView StartDream(InteractiveRuntime* rt) {
  if (!rt->Memory || !rt->MemoryEnabled) return SVL("Memory is off; run /memory on first.");
  if (!Dream_SpawnBackground(&rt->CommandMem, rt->WorkingDirectory))
    return SVL("Dreaming could not be started.");
  return {};
}

constexpr u32 kWatchPollMs = 250;

void AppendCount(StrBuf* out, u64 count, const char* one, const char* many) {
  StrBuf_AppendU64(out, count);
  StrBuf_AppendCStr(out, count == 1 ? one : many);
}

StrView WatchStatus(Arena* arena, DreamWatchState state, const DreamProgress* progress,
                    u32 shown) {
  StrBuf status = StrBuf_Make(arena, 64);
  switch (state) {
    case DreamWatchState::Starting: return SVL("starting");
    case DreamWatchState::Preparing: return SVL("preparing session digests");
    case DreamWatchState::Dreaming:
      AppendCount(&status, shown, " message", " messages");
      return StrBuf_View(&status);
    case DreamWatchState::Finished:
      if (progress->Outcome == DreamOutcome::Failed) return SVL("failed");
      StrBuf_AppendCStr(&status, "finished, ");
      AppendCount(&status, progress->PagesChanged, " page changed", " pages changed");
      return StrBuf_View(&status);
    case DreamWatchState::Stopped: return SVL("stopped before finishing");
    case DreamWatchState::NotStarted: return SVL("did not start");
  }
  return {};
}

// Adds what the dream's session has recorded since the last call. The log is
// only reread when it has grown, and only the new messages reach the TUI, so
// the transcript is built once however long the dream runs.
void ShowNewMessages(Tui* tui, Arena* scratch, JsonlRepository* reader,
                     const DreamProgress* progress, DurableId session, u64* logBytes,
                     u32* shown) {
  FsStat log = Fs_Stat(
      JsonlRepository_SessionPath(scratch, reader, progress->ProjectKey, session));
  if (!log.Exists || log.Size == *logBytes) return;
  SessionRepository sessions = JsonlRepository_AsRepository(reader);
  Array<Message> messages = {};
  if (!SessionResult_IsOk(Dream_ReadMessages(scratch, &sessions, progress, &messages)))
    return;
  *logBytes = log.Size;
  if (messages.Count <= *shown) return;
  Tui_AppendTranscript(tui, messages.Data + *shown, messages.Count - *shown);
  *shown = messages.Count;
}

StrView WatchResult(Arena* arena, DreamWatchState state, const DreamProgress* progress,
                    bool* failed) {
  *failed = state == DreamWatchState::Stopped || state == DreamWatchState::NotStarted ||
            (state == DreamWatchState::Finished &&
             progress->Outcome == DreamOutcome::Failed);
  StrBuf message = StrBuf_Make(arena, 256);
  switch (state) {
    case DreamWatchState::Finished:
      if (progress->Outcome == DreamOutcome::Dreamed) {
        StrBuf_AppendCStr(&message, "The dream finished: ");
        AppendCount(&message, progress->Digests, " session digest, ", " session digests, ");
        AppendCount(&message, progress->PagesChanged, " page changed.", " pages changed.");
        StrBuf_AppendCStr(&message, " New sessions and /reload see the result.");
      } else if (progress->Outcome == DreamOutcome::Failed) {
        StrBuf_AppendCStr(&message, "The dream failed: ");
        StrBuf_Append(&message, progress->Message);
      } else {
        StrBuf_Append(&message, progress->Message);
      }
      return StrBuf_View(&message);
    case DreamWatchState::Stopped:
      return SVL("The dream stopped before it finished; /dream watch starts another.");
    case DreamWatchState::NotStarted:
      return SVL("The dream did not start. Run clanker --dream in a terminal to see why.");
    default:
      return SVL("Stopped watching; the dream continues in the background. /dream "
                 "watch returns to it, and /memory shows when it finishes.");
  }
}

// Shows the dream's session in place of the transcript, read-only and live,
// until the user presses Ctrl+C or submits anything. A dream that ends without
// running its agent has nothing to show, so the watch ends with it.
AppCommandHandlerResult Watch(InteractiveRuntime* rt, const AppCommandUx* ux,
                              const WikiLocation* location) {
  Tui* tui = rt->TuiView;
  if (!tui)
    return Failed(SVL("Watching a dream needs the full-screen TUI; run clanker --dream "
                      "to dream in the foreground."));
  if (!rt->JsonlSessions)
    return Failed(SVL("Watching a dream needs session storage, which --no-session turns off."));
  if (!rt->Harness || rt->Harness->Running)
    return Failed(SVL("A dream can only be watched while the agent is idle."));
  Arena* arena = &rt->CommandMem;
  bool running = false;
  DreamProgress progress = {};
  Dream_ReadProgress(arena, location, &running, &progress);
  u64 since = 0;
  if (!running) {
    since = NowMillis();
    StrView error = StartDream(rt);
    if (error.Length) return Failed(error);
  }

  // A private reader, so polling leaves nothing behind in the live store.
  JsonlRepository reader = {};
  JsonlRepository_Init(&reader, rt->JsonlSessions->Root);
  Arena scratch = ArenaCreate();
  DurableId session = {};
  u32 shown = 0;
  u64 logBytes = 0;
  bool settled = false;
  Tui_ReplaceTranscript(tui, nullptr, 0, true);
  volatile i32* abort =
      RuntimeBeginOperation(rt, ux, SVL("Watching the dream (ctrl+c to return)"));
  for (;;) {
    // Once the dream is over its log is final, and only the user ends the watch.
    if (!settled) {
      ArenaReset(&scratch);
      bool held = false;
      DreamProgress now = {};
      bool read = Dream_ReadProgress(&scratch, location, &held, &now);
      DreamWatchState state =
          Dream_WatchState(held, read ? &now : nullptr, since, NowMillis());
      settled = state == DreamWatchState::Finished ||
                state == DreamWatchState::Stopped ||
                state == DreamWatchState::NotStarted;
      DurableId current = {};
      if (state != DreamWatchState::Starting &&
          DurableId_Parse(now.SessionId, &current)) {
        if (!DurableId_Eq(current, session)) {
          if (shown) Tui_ReplaceTranscript(tui, nullptr, 0, true);
          session = current;
          shown = 0;
          logBytes = 0;
        }
        ShowNewMessages(tui, &scratch, &reader, &now, session, &logBytes, &shown);
      }
      RuntimeSetOperationStatus(rt, ux, WatchStatus(&scratch, state, &now, shown));
      if (settled && !shown) break;
    }
    if (Atomic_LoadI32(abort) || Tui_SubmissionPending(tui, kWatchPollMs)) break;
  }
  RuntimeEndOperation(rt);
  ArenaRelease(&scratch);
  JsonlRepository_Destroy(&reader);

  Arena snapshotMem = ArenaCreate();
  AgentTranscriptSnapshot snapshot = {};
  if (SessionResult_IsOk(AgentHarness_SnapshotTranscript(&snapshotMem, rt->Harness, 0,
                                                         &snapshot)))
    Tui_ReplaceTranscript(tui, snapshot.Transcript.Data, snapshot.Transcript.Count, true);
  ArenaRelease(&snapshotMem);

  bool held = false;
  bool read = Dream_ReadProgress(arena, location, &held, &progress);
  DreamWatchState state =
      Dream_WatchState(held, read ? &progress : nullptr, since, NowMillis());
  bool failed = false;
  StrView message = WatchResult(arena, state, &progress, &failed);
  return failed ? Failed(message) : Succeeded(message);
}

}  // namespace

AppCommandHandlerResult HandleMemoryCommand(void* user, const AppCommandUx*,
                                            StrView arguments) {
  InteractiveRuntime* rt = (InteractiveRuntime*)user;
  if (StrView_Eq(arguments, SVL("on"))) return SetMemory(rt, true);
  if (StrView_Eq(arguments, SVL("off"))) return SetMemory(rt, false);
  WikiLocation location = {};
  if (!CurrentWiki(rt, &rt->CommandMem, &location))
    return Failed(SVL("The memory wiki could not be located."));
  if (!arguments.Length) return Status(rt, &location);
  if (StrView_Eq(arguments, SVL("lint"))) return Lint(rt, &location);
  if (StrView_Eq(arguments, SVL("restore"))) return Restore(rt, &location);
  return Failed(AppCommand_Usage(AppCommandKind::Memory));
}

AppCommandHandlerResult HandleDreamCommand(void* user, const AppCommandUx* ux,
                                           StrView arguments) {
  InteractiveRuntime* rt = (InteractiveRuntime*)user;
  bool watch = StrView_Eq(arguments, SVL("watch"));
  if (arguments.Length && !watch) return Failed(AppCommand_Usage(AppCommandKind::Dream));
  WikiLocation location = {};
  if (!CurrentWiki(rt, &rt->CommandMem, &location))
    return Failed(SVL("The memory wiki could not be located."));
  if (watch) return Watch(rt, ux, &location);
  bool running = false;
  DreamProgress progress = {};
  Dream_ReadProgress(&rt->CommandMem, &location, &running, &progress);
  if (running)
    return Failed(SVL("A dream is already running for this repository; /dream watch "
                      "follows it."));
  StrView error = StartDream(rt);
  if (error.Length) return Failed(error);
  return Succeeded(
      SVL("Dreaming in the background. /dream watch follows it, /memory shows when "
          "it finishes, and new sessions and /reload see the result."));
}
