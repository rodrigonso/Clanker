#include "app/session_switch.h"

#include <stdio.h>
#include <string.h>

#include "app/auth_flow.h"
#include "app/config_paths.h"
#include "app/event_pump.h"
#include "app/plain_select.h"
#include "base/sysinfo.h"
#include "session/metadata.h"
#include "tools/fs.h"

bool RuntimeRestartWatch(InteractiveRuntime* rt,
                                const AppCommandUx* ux) {
  if (!rt->EventPump) return true;
  AgentHarnessSnapshot snapshot = {};
  SessionResult watched =
      StartHarnessEventPump(rt->Mem, rt->Harness, rt->EventSink,
                            rt->TuiView, rt->EventPump, &snapshot);
  if (!SessionResult_IsOk(watched)) {
    EmitCommandError(ux, watched.Message);
    return false;
  }
  if (rt->TuiView)
    Tui_ReplaceTranscript(rt->TuiView, snapshot.Transcript.Data,
                          snapshot.Transcript.Count);
  return true;
}

// Writes the menu into `outOptions`/`outChoices` (each at least
// TRUST_CHOICE_CAPACITY long) and returns the number of entries. `/trust`
// exists to persist a decision, so it omits the session-only choices.
u32 BuildTrustChoices(Arena* arena, StrView parent, bool includeSessionOnly,
                             StrView* outOptions, TrustChoice* outChoices) {
  u32 count = 0;
  outOptions[count] = SVL("Trust this folder");
  outChoices[count++] = TrustChoice::TrustCurrent;
  if (parent.Length) {
    StrBuf parentOption = StrBuf_Make(arena, parent.Length + 32);
    StrBuf_AppendCStr(&parentOption, "Trust parent folder: ");
    StrBuf_Append(&parentOption, parent);
    outOptions[count] = StrBuf_View(&parentOption);
    outChoices[count++] = TrustChoice::TrustParent;
  }
  if (includeSessionOnly) {
    outOptions[count] = SVL("Trust for this session only");
    outChoices[count++] = TrustChoice::TrustSession;
  }
  outOptions[count] = SVL("Do not trust this folder");
  outChoices[count++] = TrustChoice::UntrustCurrent;
  if (includeSessionOnly) {
    outOptions[count] = SVL("Do not trust for this session only");
    outChoices[count++] = TrustChoice::UntrustSession;
  }
  return count;
}

TrustDecision ResolveTrustChoice(TrustChoice choice, StrView cwd, StrView parent) {
  TrustDecision decision = {};
  decision.Trusted = choice == TrustChoice::TrustCurrent ||
                     choice == TrustChoice::TrustParent ||
                     choice == TrustChoice::TrustSession;
  decision.Persist =
      choice != TrustChoice::TrustSession && choice != TrustChoice::UntrustSession;
  bool parentScoped = choice == TrustChoice::TrustParent;
  decision.DecisionPath = parentScoped ? parent : cwd;
  decision.RemovePath = parentScoped ? cwd : StrView{};
  return decision;
}

SessionResult ResolveRuntimeProjectTrust(
    InteractiveRuntime* rt, Arena* scratch, StrView workingDirectory,
    bool* trusted, bool* hasResources) {
  *hasResources =
      ProjectTrust_HasGatedResources(scratch, workingDirectory);
  if (!*hasResources) {
    *trusted = true;
    return SessionResult_Ok();
  }
  if (rt->TrustOverride != ProjectTrustOverride::Unspecified) {
    *trusted = rt->TrustOverride == ProjectTrustOverride::Trusted;
    return SessionResult_Ok();
  }

  ProjectTrustStore store = {};
  ProjectTrustStoreLoadResult loaded =
      ProjectTrustStore_Load(scratch, rt->TrustPath, &store);
  if (loaded == ProjectTrustStoreLoadResult::Malformed)
    return SessionResult_Error(SessionStatus::Corrupt,
                               SVL("the project trust store is malformed"));
  if (loaded == ProjectTrustStoreLoadResult::ReadFailure)
    return SessionResult_Error(SessionStatus::IoError,
                               SVL("the project trust store could not be read"));

  StrView cwd = ProjectTrust_CanonicalPath(scratch, workingDirectory);
  bool interactive = rt->TuiView || Term_IsInteractive();
  ProjectTrustResolution resolution = ProjectTrust_Resolve(
      true, rt->TrustOverride, &store, cwd, rt->DefaultTrust, interactive);
  if (!resolution.NeedsPrompt) {
    *trusted = resolution.Trusted;
    return SessionResult_Ok();
  }

  StrView parent = ProjectTrust_ParentPath(scratch, cwd);
  StrView options[TRUST_CHOICE_CAPACITY] = {};
  TrustChoice actions[TRUST_CHOICE_CAPACITY] = {};
  u32 count = BuildTrustChoices(scratch, parent, true, options, actions);

  StrBuf title = StrBuf_Make(scratch, cwd.Length + 96);
  StrBuf_AppendCStr(
      &title,
      "The selected project contains prompt files that can replace system instructions:\n");
  StrBuf_Append(&title, cwd);
  u32 selected = 0;
  RuntimeInputResult input =
      RuntimeSelect(rt, StrBuf_View(&title), options, count, &selected);
  if (input != RuntimeInputResult::Confirmed)
    return SessionResult_Error(SessionStatus::Cancelled,
                               SVL("project trust selection was cancelled"));

  TrustDecision decision = ResolveTrustChoice(actions[selected], cwd, parent);
  *trusted = decision.Trusted;
  if (!decision.Persist) return SessionResult_Ok();

  StrBuf error = StrBuf_Make(scratch, 128);
  if (!ProjectTrustStore_Update(scratch, rt->TrustPath, decision.DecisionPath,
                                decision.Trusted, decision.RemovePath, &error))
    return SessionResult_Error(SessionStatus::IoError,
                               StrView_Dup(&rt->CommandMem, StrBuf_View(&error)));
  return SessionResult_Ok();
}

bool RuntimeRestorePreviousSession(
    InteractiveRuntime* rt, DurableId previousSessionId,
    const AgentHarnessConfig* config, const AppCommandUx* ux) {
  Arena scratch = ArenaCreate((usize)32 << 20);
  SessionLog log = {};
  SessionInfo info = {};
  SessionOpenOptions open = {};
  open.SessionId = previousSessionId;
  open.ProjectKey = rt->ProjectKey;
  open.Writable = true;
  SessionResult restored =
      rt->Sessions->Open(rt->Sessions->Ctx, &scratch, &open, &log, &info);
  // A session that never received a user message was never written, so closing
  // it earlier in the switch discarded it for good. It held nothing durable by
  // definition, so a fresh session restores a usable runtime instead of
  // leaving the caller with no log at all.
  if (restored.Status == SessionStatus::NotFound) {
    SessionCreateOptions create = {};
    create.ProjectKey = rt->ProjectKey;
    create.WorkingDirectory = rt->WorkingDirectory;
    restored =
        rt->Sessions->Create(rt->Sessions->Ctx, &scratch, &create, &log, &info);
  }
  if (SessionResult_IsOk(restored)) {
    restored = AgentHarness_Init(rt->Harness, log, config);
  }
  bool ok = SessionResult_IsOk(restored) && RuntimeRestartWatch(rt, ux);
  ArenaRelease(&scratch);
  return ok;
}

bool RuntimeRollbackSessionSwitch(
    InteractiveRuntime* rt, StrView previousWorkingDirectory,
    DurableId previousSessionId, const AgentHarnessConfig* previousConfig,
    const AppCommandUx* ux) {
  if (!Sys_SetCurrentDir(previousWorkingDirectory))
    return false;
  bool restored = RuntimeRestorePreviousSession(
      rt, previousSessionId, previousConfig, ux);
  if (restored && rt->TuiView)
    Tui_LoadStartupContext(rt->TuiView);
  return restored;
}

// Points the shell tool at the session's artifact directory, so output that
// overflows the truncation limit is written beside the log it belongs to.
// Leaves the view empty when there is no durable backend (`--no-session`),
// which falls the tool back to the OS temp directory.
void RuntimeUpdateShellDirectory(InteractiveRuntime* rt,
                                        StrView projectKey,
                                        DurableId sessionId) {
  if (!rt->Shell) return;
  rt->Shell->Directory =
      rt->JsonlSessions ? JsonlRepository_SessionDir(rt->Mem, rt->JsonlSessions,
                                                     projectKey, sessionId)
                        : StrView{};
}

SessionResult RuntimeSwitchToLog(InteractiveRuntime* rt,
                                        SessionLog target,
                                        const SessionInfo* targetInfo,
                                        const SessionReduction* targetState,
                                        const AppCommandUx* ux) {
  if (!targetInfo || !targetInfo->WorkingDirectory.Length ||
      !targetInfo->ProjectKey.Length) {
    SessionLog_Close(&target);
    return SessionResult_Error(SessionStatus::InvalidArgument,
                               SVL("target session metadata is incomplete"));
  }
  if (!Fs_IsDir(targetInfo->WorkingDirectory)) {
    SessionLog_Close(&target);
    return SessionResult_Error(
        SessionStatus::NotFound,
        SVL("the session working directory no longer exists"));
  }

  Arena scratch = ArenaCreate((usize)32 << 20);
  SessionReduction previousState = {};
  SessionResult previous =
      AgentHarness_Snapshot(&scratch, rt->Harness, &previousState);
  if (!SessionResult_IsOk(previous) || previousState.RecordCount == 0) {
    SessionLog_Close(&target);
    ArenaRelease(&scratch);
    return SessionResult_IsOk(previous)
               ? SessionResult_Error(SessionStatus::InvalidState,
                                     SVL("current session has no durable root"))
               : previous;
  }
  DurableId previousSessionId = previousState.Records[0].SessionId;
  AgentHarnessConfig previousConfig = rt->Harness->Config;
  AgentHarnessConfig config = previousConfig;
  StrView previousWorkingDirectory = rt->WorkingDirectory;
  bool switchingProject =
      !StrView_Eq(targetInfo->ProjectKey, rt->ProjectKey) ||
      !StrView_Eq(targetInfo->WorkingDirectory, rt->WorkingDirectory);
  PromptContext nextPrompt = {};
  PromptContextOptions nextPromptOptions = rt->PromptOptions;
  bool nextTrusted = rt->ProjectTrusted;
  bool nextHasResources = rt->HasProjectResources;
  if (switchingProject) {
    SessionResult trust = ResolveRuntimeProjectTrust(
        rt, &scratch, targetInfo->WorkingDirectory, &nextTrusted,
        &nextHasResources);
    if (!SessionResult_IsOk(trust)) {
      SessionLog_Close(&target);
      ArenaRelease(&scratch);
      return trust;
    }
    nextPromptOptions.WorkingDirectory = targetInfo->WorkingDirectory;
    nextPromptOptions.ProjectTrusted = nextTrusted;
    StrBuf error = StrBuf_Make(&scratch, 160);
    if (!PromptContext_Build(&nextPromptOptions, &nextPrompt, &error)) {
      SessionLog_Close(&target);
      StrView message = StrView_Dup(&rt->CommandMem, StrBuf_View(&error));
      ArenaRelease(&scratch);
      return SessionResult_Error(SessionStatus::IoError, message);
    }
    config.SystemPrompt = nextPrompt.SystemPrompt;
  }
  const ProviderSpec* targetSpec = rt->Spec;
  Model targetModel = rt->CurrentModel;
  ModelRequestProfile targetProfile = config.Stream.Profile;
  if (targetState && targetState->Provider.Length &&
      targetState->Model.Length) {
    {
      targetSpec = ProviderRegistry_Find(targetState->Provider);
      if (!targetSpec || !targetSpec->CreateModel ||
          !targetSpec->CreateProvider) {
        SessionLog_Close(&target);
        PromptContext_Release(&nextPrompt);
        ArenaRelease(&scratch);
        return SessionResult_Error(
            SessionStatus::InvalidState,
            SVL("the target session uses an unavailable provider"));
      }
      targetModel = targetSpec->CreateModel(rt->Mem, targetState->Model);
      targetProfile = Model_ResolveRequestPreference(
          &targetModel, rt->PreferredProfile);
      config.ModelInfo = targetModel;
      config.ProviderImpl = targetSpec->CreateProvider(rt->Mem);
      config.Stream.Auth = {};
      config.Stream.Profile = targetProfile;
    }
  }

  if (rt->EventPump) StopHarnessEventPump(rt->EventPump);
  SessionResult closed = AgentHarness_Close(rt->Harness);
  if (!SessionResult_IsOk(closed)) {
    SessionLog_Close(&target);
    PromptContext_Release(&nextPrompt);
    RuntimeRestartWatch(rt, ux);
    ArenaRelease(&scratch);
    return closed;
  }

  if (!Sys_SetCurrentDir(targetInfo->WorkingDirectory)) {
    SessionLog_Close(&target);
    bool restored = RuntimeRollbackSessionSwitch(
        rt, previousWorkingDirectory, previousSessionId, &previousConfig, ux);
    PromptContext_Release(&nextPrompt);
    ArenaRelease(&scratch);
    return restored
               ? SessionResult_Error(
                     SessionStatus::IoError,
                     SVL("failed to enter the session working directory"))
               : SessionResult_Error(
                     SessionStatus::Faulted,
                     SVL("failed to enter the session directory and restore the previous session"));
  }

  SessionResult initialized = AgentHarness_Init(rt->Harness, target, &config);
  if (!SessionResult_IsOk(initialized)) {
    bool restored = RuntimeRollbackSessionSwitch(
        rt, previousWorkingDirectory, previousSessionId, &previousConfig, ux);
    PromptContext_Release(&nextPrompt);
    ArenaRelease(&scratch);
    return restored
               ? initialized
               : SessionResult_Error(
                     SessionStatus::Faulted,
                     SVL("session switch failed and the previous session could not be restored"));
  }
  if (rt->TuiView) {
    if (!Tui_ClearSession(rt->TuiView)) {
      AgentHarness_Close(rt->Harness);
      bool restored = RuntimeRollbackSessionSwitch(
          rt, previousWorkingDirectory, previousSessionId, &previousConfig, ux);
      PromptContext_Release(&nextPrompt);
      ArenaRelease(&scratch);
      return restored
                 ? SessionResult_Error(
                       SessionStatus::Busy,
                       SVL("the TUI could not switch sessions"))
                 : SessionResult_Error(
                       SessionStatus::Faulted,
                       SVL("the TUI switch failed and the previous session could not be restored"));
    }
  }
  if (!RuntimeRestartWatch(rt, ux)) {
    AgentHarness_Close(rt->Harness);
    bool restored = RuntimeRollbackSessionSwitch(
        rt, previousWorkingDirectory, previousSessionId, &previousConfig, ux);
    PromptContext_Release(&nextPrompt);
    ArenaRelease(&scratch);
    return restored
               ? SessionResult_Error(
                     SessionStatus::IoError,
                     SVL("failed to watch the selected session"))
               : SessionResult_Error(
                     SessionStatus::Faulted,
                     SVL("failed to watch the selected session and restore the previous session"));
  }

  rt->Spec = targetSpec;
  rt->CurrentModel = targetModel;
  if (switchingProject) {
    PromptContext previousPrompt = *rt->Prompt;
    *rt->Prompt = nextPrompt;
    nextPrompt = {};
    rt->ProjectKey = StrView_Dup(rt->Mem, targetInfo->ProjectKey);
    rt->WorkingDirectory =
        StrView_Dup(rt->Mem, targetInfo->WorkingDirectory);
    nextPromptOptions.WorkingDirectory = rt->WorkingDirectory;
    RuntimeSetPromptOptions(rt, &nextPromptOptions);
    rt->ProjectTrusted = nextTrusted;
    rt->HasProjectResources = nextHasResources;
    rt->TrustCoversProjectResources =
        nextHasResources ||
        rt->TrustOverride != ProjectTrustOverride::Unspecified;
    PromptContext_Release(&previousPrompt);
    EmitPromptDiagnostics(rt->Prompt, ux);
  }
  if (rt->TuiView) {
    Tui_SetModelProfile(rt->TuiView, targetModel.Id, targetProfile);
    Tui_LoadStartupContext(rt->TuiView);
  }

  // The switch replaced the durable session, so later payloads would otherwise
  // keep naming a transcript that is no longer written.
  RuntimeUpdateShellDirectory(rt, targetInfo->ProjectKey, targetInfo->SessionId);
  if (g_hooks) {
    Arena hookScratch = ArenaCreate((usize)1 << 20);
    HookSessionInfo hookSession = {};
    hookSession.SessionId = DurableId_View(&targetInfo->SessionId);
    hookSession.WorkingDirectory = rt->WorkingDirectory;
    hookSession.TranscriptPath =
        JsonlRepository_SessionPath(&hookScratch, rt->JsonlSessions,
                                    targetInfo->ProjectKey, targetInfo->SessionId);
    HookRunner_SetSession(g_hooks, &hookSession);
    ArenaRelease(&hookScratch);
    // Announced as a fresh start so a consumer tracks the new session rather
    // than silently reattributing it to the old one.
    HookRunner_Dispatch(g_hooks, HookEvent::SessionStart, StrView{});
  }

  ArenaRelease(&scratch);
  return initialized;
}

StrView SessionTitle(const SessionInfo* info) {
  return info->DisplayTitle.Length ? info->DisplayTitle
                                   : SVL("(untitled session)");
}

static StrView FormatDuration(Arena* arena, u64 milliseconds) {
  u64 seconds = milliseconds / 1000;
  u64 days = seconds / 86400;
  u64 hours = (seconds / 3600) % 24;
  u64 minutes = (seconds / 60) % 60;
  seconds %= 60;
  StrBuf text = StrBuf_Make(arena, 32);
  if (days) {
    StrBuf_AppendU64(&text, days);
    StrBuf_AppendCStr(&text, "d ");
    StrBuf_AppendU64(&text, hours);
    StrBuf_AppendChar(&text, 'h');
  } else if (hours) {
    StrBuf_AppendU64(&text, hours);
    StrBuf_AppendCStr(&text, "h ");
    StrBuf_AppendU64(&text, minutes);
    StrBuf_AppendChar(&text, 'm');
  } else if (minutes) {
    StrBuf_AppendU64(&text, minutes);
    StrBuf_AppendCStr(&text, "m ");
    StrBuf_AppendU64(&text, seconds);
    StrBuf_AppendChar(&text, 's');
  } else {
    StrBuf_AppendU64(&text, seconds);
    StrBuf_AppendChar(&text, 's');
  }
  return StrBuf_View(&text);
}

static StrView FormatRelativeAge(Arena* arena, u64 timestamp, u64 now) {
  if (!timestamp || now < timestamp) return SVL("now");
  u64 seconds = (now - timestamp) / 1000;
  StrBuf text = StrBuf_Make(arena, 32);
  if (seconds < 60) {
    StrBuf_AppendU64(&text, seconds);
    StrBuf_AppendCStr(&text, "s ago");
  } else if (seconds < 3600) {
    StrBuf_AppendU64(&text, seconds / 60);
    StrBuf_AppendCStr(&text, "m ago");
  } else if (seconds < 86400) {
    StrBuf_AppendU64(&text, seconds / 3600);
    StrBuf_AppendCStr(&text, "h ago");
  } else {
    StrBuf_AppendU64(&text, seconds / 86400);
    StrBuf_AppendCStr(&text, "d ago");
  }
  return StrBuf_View(&text);
}

static StrView FormatTimestamp(Arena* arena, u64 timestamp) {
  if (!timestamp) return SVL("(unknown)");
  struct tm local = {};
  if (!Sys_LocalTime(timestamp, &local)) return SVL("(unknown)");
  char value[32];
  int length =
      snprintf(value, sizeof(value), "%04d-%02d-%02d %02d:%02d:%02d",
               local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
               local.tm_hour, local.tm_min, local.tm_sec);
  return length > 0 ? StrView_Dup(arena, StrView{value, (u32)length})
                    : SVL("(unknown)");
}

static void AddDetail(Array<StrView>* lines, Arena* arena, const char* label,
                      StrView value) {
  StrBuf line = StrBuf_Make(arena, value.Length + 24);
  StrBuf_AppendCStr(&line, label);
  StrBuf_Append(&line, value.Length ? value : SVL("(none)"));
  Array_Push(lines, StrBuf_View(&line));
}

Array<StrView> SessionDetailLines(Arena* arena,
                                         const SessionInfo* info,
                                         StrView storage, StrView status) {
  Array<StrView> lines = Array_Make<StrView>(arena, 20);
  AddDetail(&lines, arena, "Title      ", SessionTitle(info));
  AddDetail(&lines, arena, "Name       ", info->ExplicitName);
  AddDetail(&lines, arena, "ID         ", DurableId_View(&info->SessionId));
  AddDetail(&lines, arena, "Project    ", info->ProjectKey);
  AddDetail(&lines, arena, "CWD        ", info->WorkingDirectory);
  AddDetail(&lines, arena, "Storage    ", storage);
  AddDetail(&lines, arena, "Status     ", status);
  AddDetail(&lines, arena, "Created    ",
            FormatTimestamp(arena, info->CreatedAt));
  AddDetail(&lines, arena, "Activity   ",
            FormatTimestamp(arena, info->LastActivityAt));
  AddDetail(&lines, arena, "Updated    ",
            FormatTimestamp(arena, info->StorageUpdatedAt));
  AddDetail(&lines, arena, "Wall span  ",
            FormatDuration(arena, info->WallSpanMs));

  StrBuf model =
      StrBuf_Make(arena, info->CurrentProvider.Length +
                             info->CurrentModel.Length + 2);
  if (info->CurrentProvider.Length) {
    StrBuf_Append(&model, info->CurrentProvider);
    StrBuf_AppendChar(&model, '/');
  }
  StrBuf_Append(&model, info->CurrentModel);
  AddDetail(&lines, arena, "Model      ", StrBuf_View(&model));

  StrBuf messages = StrBuf_Make(arena, 96);
  StrBuf_AppendU64(&messages, info->MessageCount);
  StrBuf_AppendCStr(&messages, " total (");
  StrBuf_AppendU64(&messages, info->UserMessageCount);
  StrBuf_AppendCStr(&messages, " user, ");
  StrBuf_AppendU64(&messages, info->AssistantMessageCount);
  StrBuf_AppendCStr(&messages, " assistant, ");
  StrBuf_AppendU64(&messages, info->ToolResultMessageCount);
  StrBuf_AppendCStr(&messages, " tool)");
  AddDetail(&lines, arena, "Messages   ", StrBuf_View(&messages));

  StrBuf counts = StrBuf_Make(arena, 96);
  StrBuf_AppendU64(&counts, info->ToolCallCount);
  StrBuf_AppendCStr(&counts, " tool calls, ");
  StrBuf_AppendU64(&counts, info->CompactionCount);
  StrBuf_AppendCStr(&counts, " compactions");
  AddDetail(&lines, arena, "Counts     ", StrBuf_View(&counts));

  StrBuf tokens = StrBuf_Make(arena, 128);
  StrBuf_AppendU64(&tokens, info->TotalTokens);
  StrBuf_AppendCStr(&tokens, " total (in ");
  StrBuf_AppendU64(&tokens, info->InputTokens);
  StrBuf_AppendCStr(&tokens, ", out ");
  StrBuf_AppendU64(&tokens, info->OutputTokens);
  StrBuf_AppendCStr(&tokens, ", cache read ");
  StrBuf_AppendU64(&tokens, info->CacheReadTokens);
  StrBuf_AppendCStr(&tokens, ", write ");
  StrBuf_AppendU64(&tokens, info->CacheWriteTokens);
  StrBuf_AppendCStr(&tokens, ", reasoning ");
  StrBuf_AppendU64(&tokens, info->ReasoningTokens);
  StrBuf_AppendChar(&tokens, ')');
  AddDetail(&lines, arena, "Tokens     ", StrBuf_View(&tokens));

  if (DurableId_IsSet(info->ForkedFromSessionId)) {
    StrBuf ancestry = StrBuf_Make(arena, 64);
    StrBuf_Append(&ancestry, DurableId_View(&info->ForkedFromSessionId));
    StrBuf_AppendCStr(&ancestry, " @ ");
    StrBuf_Append(&ancestry, DurableId_View(&info->ForkedFromEntryId));
    AddDetail(&lines, arena, "Forked     ", StrBuf_View(&ancestry));
  }
  if (info->Label.Length)
    AddDetail(&lines, arena, "Branch     ", info->Label);
  return lines;
}

StrView SessionStatusText(const SessionInfo* info) {
  if (info->Busy) return SVL("busy");
  return SVL("available");
}

StrView FormatSessionOption(Arena* arena, const SessionInfo* info,
                                   DurableId currentSessionId,
                                   bool allProjects, bool includeFullId,
                                   u64 nowMs, StrView home) {
  StrBuf option =
      StrBuf_Make(arena, SessionTitle(info).Length +
                             info->WorkingDirectory.Length + 160);
  StrBuf_Append(&option, SessionTitle(info));
  StrBuf_AppendCStr(&option, "  |  ");
  StrBuf_AppendU64(&option, info->MessageCount);
  StrBuf_AppendCStr(&option, " msg  |  ");
  StrBuf_AppendU64(&option, info->TotalTokens);
  StrBuf_AppendCStr(&option, " tok  |  ");
  StrBuf_Append(&option,
                FormatRelativeAge(arena, info->LastActivityAt, nowMs));
  StrBuf_AppendCStr(&option, "  |  ");
  StrBuf_Append(&option, FormatDuration(arena, info->WallSpanMs));
  StrBuf_AppendCStr(&option, " span");
  if (allProjects) {
    StrView cwd =
        Sys_AbbreviatePath(arena, info->WorkingDirectory, home);
    StrBuf_AppendCStr(&option, "  |  ");
    StrBuf_Append(&option, cwd);
  }
  if (includeFullId) {
    StrBuf_AppendCStr(&option, "  |  ");
    StrBuf_Append(&option, DurableId_View(&info->SessionId));
  }
  if (DurableId_Eq(info->SessionId, currentSessionId))
    StrBuf_AppendCStr(&option, "  [current]");
  else if (info->Busy)
    StrBuf_AppendCStr(&option, "  [busy]");
  return StrBuf_View(&option);
}

SessionOptionList BuildSessionOptions(
    Arena* arena, Array<SessionInfo>* items, DurableId currentSessionId,
    bool allProjects, bool includeFullId, bool availableOnly, u64 nowMs,
    StrView home) {
  SessionOptionList result = {};
  u32 capacity = items->Count + 1;
  result.Sessions = ArenaPushArray(arena, SessionInfo*, capacity);
  result.Options = ArenaPushArray(arena, StrView, capacity);
  for (u32 i = 0; i < items->Count; i++) {
    SessionInfo* info = &items->Data[i];
    if (availableOnly &&
        (DurableId_Eq(info->SessionId, currentSessionId) || info->Busy))
      continue;
    result.Sessions[result.Count] = info;
    result.Options[result.Count] =
        FormatSessionOption(arena, info, currentSessionId, allProjects,
                            includeFullId, nowMs, home);
    result.Count++;
  }
  return result;
}

SessionResult CurrentSessionId(Arena* arena, InteractiveRuntime* rt,
                                      DurableId* out) {
  SessionReduction state = {};
  SessionResult snapshot =
      AgentHarness_Snapshot(arena, rt->Harness, &state);
  if (!SessionResult_IsOk(snapshot)) return snapshot;
  if (!state.RecordCount)
    return SessionResult_Error(SessionStatus::InvalidState,
                               SVL("current session has no durable root"));
  *out = state.Records[0].SessionId;
  return snapshot;
}
bool ResolveStartupProjectTrust(
    Arena* arena, StrView trustPath, StrView workingDirectory,
    DefaultProjectTrust defaultTrust, ProjectTrustOverride overrideValue,
    bool interactive, bool* trusted, bool* hasResources) {
  *hasResources =
      ProjectTrust_HasGatedResources(arena, workingDirectory);
  if (!*hasResources) {
    *trusted = true;
    return true;
  }
  if (overrideValue != ProjectTrustOverride::Unspecified) {
    *trusted = overrideValue == ProjectTrustOverride::Trusted;
    return true;
  }

  ProjectTrustStore store = {};
  ProjectTrustStoreLoadResult loaded =
      ProjectTrustStore_Load(arena, trustPath, &store);
  if (loaded == ProjectTrustStoreLoadResult::Malformed ||
      loaded == ProjectTrustStoreLoadResult::ReadFailure) {
    fprintf(stderr, "error: project trust store %.*s %s\n",
            (int)trustPath.Length, trustPath.Data,
            loaded == ProjectTrustStoreLoadResult::Malformed
                ? "is malformed"
                : "could not be read");
    return false;
  }

  StrView cwd = ProjectTrust_CanonicalPath(arena, workingDirectory);
  ProjectTrustResolution resolution =
      ProjectTrust_Resolve(true, overrideValue, &store, cwd, defaultTrust,
                           interactive);
  if (!resolution.NeedsPrompt) {
    *trusted = resolution.Trusted;
    return true;
  }

  StrView parent = ProjectTrust_ParentPath(arena, cwd);
  StrView options[TRUST_CHOICE_CAPACITY] = {};
  TrustChoice actions[TRUST_CHOICE_CAPACITY] = {};
  u32 count = BuildTrustChoices(arena, parent, true, options, actions);

  StrBuf title = StrBuf_Make(arena, cwd.Length + 96);
  StrBuf_AppendCStr(
      &title,
      "This project contains .clank prompt files that can replace system instructions:\n");
  StrBuf_Append(&title, cwd);
  u32 selected = 0;
  if (!Plain_Select(StrBuf_View(&title), options, count, &selected)) {
    *trusted = false;
    return true;
  }

  TrustDecision decision = ResolveTrustChoice(actions[selected], cwd, parent);
  *trusted = decision.Trusted;
  if (!decision.Persist) return true;

  StrBuf error = StrBuf_Make(arena, 128);
  if (!ProjectTrustStore_Update(arena, trustPath, decision.DecisionPath,
                                decision.Trusted, decision.RemovePath, &error)) {
    fprintf(stderr, "error: %.*s\n", (int)error.Length, error.Data);
    return false;
  }
  return true;
}

StrView SessionDataRoot(Arena* arena) { return ClankDataRoot(arena); }

StrView SessionProjectKey(Arena* arena, StrView workingDirectory) {
  u64 hash = 1469598103934665603ull;
  for (u32 i = 0; i < workingDirectory.Length; i++) {
    char c = workingDirectory.Data[i];
#if defined(_WIN32)
    if (c == '\\') c = '/';
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
#endif
    hash ^= (u8)c;
    hash *= 1099511628211ull;
  }

  u32 baseStart = workingDirectory.Length;
  while (baseStart > 0 && workingDirectory.Data[baseStart - 1] != '/' &&
         workingDirectory.Data[baseStart - 1] != '\\')
    baseStart--;
  StrView base = {workingDirectory.Data + baseStart,
                  workingDirectory.Length - baseStart};
  StrBuf key = StrBuf_Make(arena, base.Length + 18);
  for (u32 i = 0; i < base.Length; i++) {
    char c = base.Data[i];
#if defined(_WIN32)
    // The hash half of the key already folds case, so the readable half has to
    // fold it too. Otherwise one directory reached as C:\Work\Clank and as
    // c:/work/clank hashes identically but yields two different keys, and the
    // same project gets two session directories.
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
#endif
    bool valid = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                 c == '.';
    StrBuf_AppendChar(&key, valid ? c : '-');
  }
  if (key.Length == 0) StrBuf_AppendCStr(&key, "project");
  char suffix[24];
  snprintf(suffix, sizeof(suffix), "-%016llx", (unsigned long long)hash);
  StrBuf_AppendCStr(&key, suffix);
  return StrBuf_View(&key);
}
