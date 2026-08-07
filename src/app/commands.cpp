#include "app/commands.h"

#include <stdio.h>
#include <string.h>

#include "app/auth_flow.h"
#include "app/event_pump.h"
#include "app/hook_runner.h"
#include "app/plain_select.h"
#include "app/session_switch.h"
#include "app/settings.h"
#include "base/sysinfo.h"
#include "session/metadata.h"
#include "tools/fs.h"

static AppCommandHandlerResult HandleSessionCommand(
    void* user, const AppCommandUx*, StrView) {
  InteractiveRuntime* rt = (InteractiveRuntime*)user;
  Arena scratch = ArenaCreate((usize)32 << 20);
  SessionReduction state = {};
  SessionResult snapshot =
      AgentHarness_Snapshot(&scratch, rt->Harness, &state);
  if (!SessionResult_IsOk(snapshot)) {
    ArenaRelease(&scratch);
    return CommandResult(snapshot, {});
  }
  SessionMetadataBuildOptions build = {};
  build.Busy = rt->Harness->Running;
  build.NowMs = NowMillis();
  SessionInfo info = {};
  SessionResult metadata =
      SessionMetadata_Build(&scratch, state.Records, state.RecordCount,
                            &build, &info);
  if (!SessionResult_IsOk(metadata)) {
    ArenaRelease(&scratch);
    return CommandResult(metadata, {});
  }
  StrView status = rt->Harness->Faulted
                       ? SVL("faulted")
                   : rt->Harness->Running
                       ? SVL("running")
                       : SVL("idle");
  Array<StrView> lines = SessionDetailLines(
      &scratch, &info, rt->NoSession ? SVL("memory") : SVL("jsonl"),
      status);
  if (rt->TuiView) {
    u32 selected = 0;
    RuntimeInputResult result =
        RuntimeSelect(rt, SVL("Current session"), lines.Data, lines.Count,
                      &selected);
    ArenaRelease(&scratch);
    if (result == RuntimeInputResult::Back)
      return ReturnToCommandPalette(rt);
    return AppCommandHandlerResult{AppCommandHandlerStatus::Success, {}};
  }
  StrBuf text = StrBuf_Make(rt->Mem, 768);
  for (u32 i = 0; i < lines.Count; i++) {
    StrBuf_Append(&text, lines.Data[i]);
    if (i + 1 < lines.Count) StrBuf_AppendChar(&text, '\n');
  }
  ArenaRelease(&scratch);
  return AppCommandHandlerResult{AppCommandHandlerStatus::Success,
                                 StrBuf_View(&text)};
}

static AppCommandHandlerResult HandleNameCommand(
    void* user, const AppCommandUx*, StrView arguments) {
  InteractiveRuntime* rt = (InteractiveRuntime*)user;
  Arena scratch = ArenaCreate((usize)16 << 20);
  StrView name = arguments;
  if (!arguments.Length) {
    if (!rt->TuiView) {
      ArenaRelease(&scratch);
      return AppCommandHandlerResult{
          AppCommandHandlerStatus::Failed,
          AppCommand_Usage(AppCommandKind::Name)};
    }
    SessionReduction state = {};
    SessionResult snapshot =
        AgentHarness_Snapshot(&scratch, rt->Harness, &state);
    if (!SessionResult_IsOk(snapshot)) {
      ArenaRelease(&scratch);
      return CommandResult(snapshot, {});
    }
    SessionMetadataBuildOptions build = {};
    SessionInfo info = {};
    SessionResult metadata =
        SessionMetadata_Build(&scratch, state.Records, state.RecordCount,
                              &build, &info);
    if (!SessionResult_IsOk(metadata)) {
      ArenaRelease(&scratch);
      return CommandResult(metadata, {});
    }
    StrView entered = {};
    RuntimeInputResult result =
        RuntimeReadText(rt, &scratch, SVL("Session name"),
                        SVL("Name (empty clears)"), info.ExplicitName,
                        &entered);
    if (result != RuntimeInputResult::Confirmed) {
      ArenaRelease(&scratch);
      if (result == RuntimeInputResult::Back)
        return ReturnToCommandPalette(rt);
      return AppCommandHandlerResult{AppCommandHandlerStatus::Cancelled,
                                     SVL("Session name unchanged.")};
    }
    name = TrimAscii(entered);
  } else if (StrView_Eq(arguments, SVL("--clear"))) {
    name = {};
  }
  SessionResult named = AgentHarness_SetName(rt->Harness, name);
  ArenaRelease(&scratch);
  return CommandResult(named, name.Length ? SVL("session name updated")
                                          : SVL("session name cleared"));
}

static SessionResult ListSessions(InteractiveRuntime* rt, Arena* arena,
                                  SessionListScope scope,
                                  Array<SessionInfo>* out,
                                  u32* skippedCount = nullptr) {
  SessionListOptions options = {};
  options.Scope = scope;
  options.ProjectKey = rt->ProjectKey;
  options.SkippedCount = skippedCount;
  return rt->Sessions->List(rt->Sessions->Ctx, arena, &options, out);
}

static void ReportSkippedSessions(const AppCommandUx* ux, Arena* arena,
                                  u32 skippedCount) {
  if (!skippedCount) return;
  StrBuf warning = StrBuf_Make(arena, 96);
  StrBuf_AppendCStr(&warning, "Skipped ");
  StrBuf_AppendU64(&warning, skippedCount);
  StrBuf_AppendCStr(&warning, skippedCount == 1
                                 ? " unreadable session file."
                                 : " unreadable session files.");
  EmitCommandNotice(ux, StrBuf_View(&warning));
}

static AppCommandHandlerResult HandleSessionsCommand(
    void* user, const AppCommandUx* ux, StrView arguments) {
  InteractiveRuntime* rt = (InteractiveRuntime*)user;
  Arena scratch = ArenaCreate((usize)32 << 20);
  SessionListScope scope = SessionListScope::CurrentProject;
  if (arguments.Length) {
    if (!StrView_Eq(arguments, SVL("all"))) {
      ArenaRelease(&scratch);
      return AppCommandHandlerResult{AppCommandHandlerStatus::Failed,
                                     AppCommand_Usage(
                                         AppCommandKind::Sessions)};
    }
    scope = SessionListScope::AllProjects;
  }
list_scope:
  Array<SessionInfo> items = {};
  u32 skippedCount = 0;
  SessionResult listed =
      ListSessions(rt, &scratch, scope, &items, &skippedCount);
  if (!SessionResult_IsOk(listed)) {
    ArenaRelease(&scratch);
    return CommandResult(listed, {});
  }
  ReportSkippedSessions(ux, &scratch, skippedCount);
  DurableId currentSessionId = {};
  SessionResult current =
      CurrentSessionId(&scratch, rt, &currentSessionId);
  if (!SessionResult_IsOk(current)) {
    ArenaRelease(&scratch);
    return CommandResult(current, {});
  }
  bool allProjects = scope == SessionListScope::AllProjects;
  u64 nowMs = NowMillis();
  StrView home = allProjects ? Sys_HomeDir(&scratch) : StrView{};
  SessionOptionList rows = BuildSessionOptions(
      &scratch, &items, currentSessionId, allProjects, !rt->TuiView, false,
      nowMs, home);
  u32 optionCount = rows.Count;
  if (rt->TuiView && !allProjects)
    rows.Options[optionCount++] = SVL("All projects...");
  else if (!optionCount)
    rows.Options[optionCount++] = SVL("No sessions found");

  if (rt->TuiView) {
    u32 selected = 0;
    RuntimeInputResult result =
        RuntimeSelect(rt, allProjects ? SVL("All sessions")
                                      : SVL("Project sessions"),
                      rows.Options, optionCount,
                      &selected);
    if (result == RuntimeInputResult::Back) {
      ArenaRelease(&scratch);
      return ReturnToCommandPalette(rt);
    }
    if (result != RuntimeInputResult::Confirmed) {
      ArenaRelease(&scratch);
      return AppCommandHandlerResult{AppCommandHandlerStatus::Success, {}};
    }
    if (!allProjects && selected == rows.Count) {
      scope = SessionListScope::AllProjects;
      goto list_scope;
    }
    if (selected < rows.Count) {
      Array<StrView> details =
          SessionDetailLines(&scratch, rows.Sessions[selected],
                             rt->NoSession ? SVL("memory") : SVL("jsonl"),
                             SessionStatusText(rows.Sessions[selected]));
      u32 ignored = 0;
      RuntimeSelect(rt, SVL("Session details"), details.Data, details.Count,
                    &ignored);
    }
    ArenaRelease(&scratch);
    return AppCommandHandlerResult{AppCommandHandlerStatus::Success, {}};
  }

  StrBuf text = StrBuf_Make(rt->Mem, optionCount * 96 + 32);
  for (u32 i = 0; i < optionCount; i++) {
    StrBuf_Append(&text, rows.Options[i]);
    if (i + 1 < optionCount) StrBuf_AppendChar(&text, '\n');
  }
  ArenaRelease(&scratch);
  return AppCommandHandlerResult{AppCommandHandlerStatus::Success,
                                 StrBuf_View(&text)};
}

static AppCommandHandlerResult HandleResumeCommand(
    void* user, const AppCommandUx* ux, StrView arguments) {
  InteractiveRuntime* rt = (InteractiveRuntime*)user;
  if (rt->Harness->Running)
    return AppCommandHandlerResult{AppCommandHandlerStatus::Failed,
                                   SVL("cannot switch sessions while an operation is active")};
  Arena scratch = ArenaCreate((usize)64 << 20);
  DurableId sessionId = {};
  SessionInfo selectedInfo = {};
  DurableId currentSessionId = {};
  SessionResult current =
      CurrentSessionId(&scratch, rt, &currentSessionId);
  if (!SessionResult_IsOk(current)) {
    ArenaRelease(&scratch);
    return CommandResult(current, {});
  }
  if (arguments.Length) {
    if (!DurableId_Parse(arguments, &sessionId)) {
      ArenaRelease(&scratch);
      return AppCommandHandlerResult{AppCommandHandlerStatus::Failed,
                                     SVL("invalid session ID")};
    }
    SessionResult resolved = rt->Sessions->Resolve(
        rt->Sessions->Ctx, &scratch, sessionId, &selectedInfo);
    if (!SessionResult_IsOk(resolved)) {
      ArenaRelease(&scratch);
      return CommandResult(resolved, {});
    }
  } else {
    if (!rt->TuiView) {
      ArenaRelease(&scratch);
      return AppCommandHandlerResult{AppCommandHandlerStatus::Failed,
                                     AppCommand_Usage(
                                         AppCommandKind::Resume)};
    }
    SessionListScope scope = SessionListScope::CurrentProject;
resume_scope:
    Array<SessionInfo> items = {};
    u32 skippedCount = 0;
    SessionResult listed =
        ListSessions(rt, &scratch, scope, &items, &skippedCount);
    if (!SessionResult_IsOk(listed)) {
      ArenaRelease(&scratch);
      return CommandResult(listed, {});
    }
    ReportSkippedSessions(ux, &scratch, skippedCount);
    bool allProjects = scope == SessionListScope::AllProjects;
    u64 nowMs = NowMillis();
    StrView home = allProjects ? Sys_HomeDir(&scratch) : StrView{};
    SessionOptionList rows = BuildSessionOptions(
        &scratch, &items, currentSessionId, allProjects, false, true, nowMs,
        home);
    u32 optionCount = rows.Count;
    if (!allProjects)
      rows.Options[optionCount++] = SVL("All projects...");
    else if (!optionCount)
      rows.Options[optionCount++] = SVL("No other sessions are available");
    u32 selected = 0;
    RuntimeInputResult result =
        RuntimeSelect(rt, allProjects ? SVL("Resume from all projects")
                                      : SVL("Resume session"),
                      rows.Options, optionCount,
                      &selected);
    if (result != RuntimeInputResult::Confirmed) {
      ArenaRelease(&scratch);
      if (result == RuntimeInputResult::Back)
        return ReturnToCommandPalette(rt);
      return AppCommandHandlerResult{AppCommandHandlerStatus::Success, {}};
    }
    if (!allProjects && selected == rows.Count) {
      scope = SessionListScope::AllProjects;
      goto resume_scope;
    }
    if (selected >= rows.Count) {
      ArenaRelease(&scratch);
      return AppCommandHandlerResult{AppCommandHandlerStatus::Success, {}};
    }
    selectedInfo = *rows.Sessions[selected];
    sessionId = selectedInfo.SessionId;
  }
  if (DurableId_Eq(sessionId, currentSessionId)) {
    ArenaRelease(&scratch);
    return AppCommandHandlerResult{AppCommandHandlerStatus::Failed,
                                   SVL("that session is already current")};
  }
  if (selectedInfo.Busy) {
    ArenaRelease(&scratch);
    return AppCommandHandlerResult{AppCommandHandlerStatus::Failed,
                                   SVL("session already has a writer")};
  }
  SessionOpenOptions open = {};
  open.SessionId = sessionId;
  open.ProjectKey = selectedInfo.ProjectKey;
  open.Writable = true;
  SessionLog log = {};
  SessionInfo info = {};
  SessionResult opened =
      rt->Sessions->Open(rt->Sessions->Ctx, &scratch, &open, &log, &info);
  if (!SessionResult_IsOk(opened)) {
    ArenaRelease(&scratch);
    return CommandResult(opened, {});
  }
  Array<SessionRecord> records = {};
  SessionReduction state = {};
  SessionResult read = SessionLog_ReadAll(&log, &scratch, &records);
  if (SessionResult_IsOk(read))
    read = SessionReducer_Reduce(&scratch, records.Data, records.Count, &state);
  if (!SessionResult_IsOk(read)) {
    SessionLog_Close(&log);
    ArenaRelease(&scratch);
    return CommandResult(read, {});
  }
  SessionResult switched =
      RuntimeSwitchToLog(rt, log, &info, &state, ux);
  ArenaRelease(&scratch);
  return CommandResult(switched, SVL("session resumed"));
}

static AppCommandHandlerResult HandleTreeCommand(
    void* user, const AppCommandUx*, StrView) {
  InteractiveRuntime* rt = (InteractiveRuntime*)user;
  Arena scratch = ArenaCreate((usize)32 << 20);
  SessionReduction state = {};
  SessionResult snapshot =
      AgentHarness_Snapshot(&scratch, rt->Harness, &state);
  if (!SessionResult_IsOk(snapshot)) {
    ArenaRelease(&scratch);
    return CommandResult(snapshot, {});
  }
  StrBuf text = StrBuf_Make(rt->Mem, state.RecordCount * 72 + 32);
  u32 treeCount = 0;
  for (u32 i = 0; i < state.RecordCount; i++) {
    const SessionRecord* record = &state.Records[i];
    if (!SessionRecord_IsTreeNode(record->Kind)) continue;
    if (treeCount++) StrBuf_AppendChar(&text, '\n');
    StrBuf_AppendCStr(&text,
                     DurableId_Eq(record->EntryId, state.HeadId) ? "* " : "  ");
    StrBuf_Append(&text, DurableId_View(&record->EntryId));
    if (record->Kind == SessionRecordKind::Message) {
      if (record->MessageEntry.Value.Kind == Role::User)
        StrBuf_AppendCStr(&text, " user");
      else if (record->MessageEntry.Value.Kind == Role::Assistant)
        StrBuf_AppendCStr(&text, " assistant");
      else
        StrBuf_AppendCStr(&text, " tool");
    } else if (record->Kind == SessionRecordKind::Compaction) {
      StrBuf_AppendCStr(&text, " compaction");
    } else if (record->Kind == SessionRecordKind::Model) {
      StrBuf_AppendCStr(&text, " model");
    } else if (record->Kind == SessionRecordKind::BranchLabel) {
      StrBuf_AppendCStr(&text, " label");
    }
    DurableId parentId = SessionRecord_ParentId(record);
    if (DurableId_IsSet(parentId)) {
      StrBuf_AppendCStr(&text, " <- ");
      StrBuf_Append(&text, DurableId_View(&parentId));
    }
  }
  if (!treeCount) StrBuf_AppendCStr(&text, "(empty tree)");
  ArenaRelease(&scratch);
  return AppCommandHandlerResult{AppCommandHandlerStatus::Success,
                                 StrBuf_View(&text)};
}

static AppCommandHandlerResult HandleCheckoutCommand(
    void* user, const AppCommandUx*, StrView arguments) {
  InteractiveRuntime* rt = (InteractiveRuntime*)user;
  DurableId entryId = {};
  if (!DurableId_Parse(arguments, &entryId))
    return AppCommandHandlerResult{AppCommandHandlerStatus::Failed,
                                   SVL("invalid entry ID")};
  return CommandResult(AgentHarness_Navigate(rt->Harness, entryId),
                       SVL("branch checked out"));
}

static AppCommandHandlerResult HandleForkCommand(
    void* user, const AppCommandUx* ux, StrView arguments) {
  InteractiveRuntime* rt = (InteractiveRuntime*)user;
  Arena scratch = ArenaCreate((usize)64 << 20);
  SessionReduction state = {};
  SessionResult snapshot =
      AgentHarness_Snapshot(&scratch, rt->Harness, &state);
  if (!SessionResult_IsOk(snapshot)) {
    ArenaRelease(&scratch);
    return CommandResult(snapshot, {});
  }
  DurableId head = state.HeadId;
  if (arguments.Length && !DurableId_Parse(arguments, &head)) {
    ArenaRelease(&scratch);
    return AppCommandHandlerResult{AppCommandHandlerStatus::Failed,
                                   SVL("invalid entry ID")};
  }
  SessionLog forkLog = {};
  SessionInfo forkInfo = {};
  SessionResult forked =
      AgentHarness_Fork(&scratch, rt->Harness, rt->Sessions, head, {}, {},
                        &forkLog, &forkInfo);
  if (SessionResult_IsOk(forked)) {
    Array<SessionRecord> records = {};
    SessionReduction forkState = {};
    forked = SessionLog_ReadAll(&forkLog, &scratch, &records);
    if (SessionResult_IsOk(forked))
      forked = SessionReducer_Reduce(&scratch, records.Data, records.Count,
                                     &forkState);
    if (SessionResult_IsOk(forked))
      forked =
          RuntimeSwitchToLog(rt, forkLog, &forkInfo, &forkState, ux);
    else
      SessionLog_Close(&forkLog);
  }
  ArenaRelease(&scratch);
  return CommandResult(forked, SVL("session forked"));
}

static AppCommandHandlerResult HandleCompactCommand(
    void* user, const AppCommandUx*, StrView) {
  InteractiveRuntime* rt = (InteractiveRuntime*)user;
  return CommandResult(AgentHarness_Compact(rt->Harness),
                       SVL("session compacted"));
}




static AppCommandHandlerResult HandleTrustCommand(
    void* user, const AppCommandUx*, StrView) {
  InteractiveRuntime* rt = (InteractiveRuntime*)user;
  Arena scratch = ArenaCreate((usize)16 << 20);
  ProjectTrustStore store = {};
  ProjectTrustStoreLoadResult loaded =
      ProjectTrustStore_Load(&scratch, rt->TrustPath, &store);
  if (loaded == ProjectTrustStoreLoadResult::Malformed ||
      loaded == ProjectTrustStoreLoadResult::ReadFailure) {
    StrView message =
        loaded == ProjectTrustStoreLoadResult::Malformed
            ? SVL("The project trust store is malformed.")
            : SVL("The project trust store could not be read.");
    AppCommandHandlerResult result = RuntimeOwnedFailure(rt, message);
    ArenaRelease(&scratch);
    return result;
  }

  StrView cwd = ProjectTrust_CanonicalPath(&scratch, rt->WorkingDirectory);
  StrView parent = ProjectTrust_ParentPath(&scratch, cwd);
  ProjectTrustMatch match = ProjectTrustStore_Find(&store, cwd);
  StrBuf title = StrBuf_Make(&scratch, cwd.Length + match.Path.Length + 160);
  StrBuf_AppendCStr(&title, "Project trust for ");
  StrBuf_Append(&title, cwd);
  StrBuf_AppendCStr(&title, "\nCurrent session: ");
  StrBuf_AppendCStr(&title, rt->ProjectTrusted ? "trusted" : "untrusted");
  StrBuf_AppendCStr(&title, "\nSaved decision: ");
  if (match.Found) {
    StrBuf_AppendCStr(&title, match.Trusted ? "trust " : "do not trust ");
    StrBuf_Append(&title, match.Path);
  } else {
    StrBuf_AppendCStr(&title, "none");
  }

  StrView options[TRUST_CHOICE_CAPACITY] = {};
  TrustChoice actions[TRUST_CHOICE_CAPACITY] = {};
  u32 count = BuildTrustChoices(&scratch, parent, false, options, actions);

  u32 selected = 0;
  RuntimeInputResult input =
      RuntimeSelect(rt, StrBuf_View(&title), options, count, &selected);
  if (input != RuntimeInputResult::Confirmed) {
    ArenaRelease(&scratch);
    return AppCommandHandlerResult{AppCommandHandlerStatus::Cancelled,
                                   SVL("Trust unchanged.")};
  }

  TrustDecision decision = ResolveTrustChoice(actions[selected], cwd, parent);
  StrBuf error = StrBuf_Make(&scratch, 128);
  if (!ProjectTrustStore_Update(&scratch, rt->TrustPath, decision.DecisionPath,
                                decision.Trusted, decision.RemovePath, &error)) {
    AppCommandHandlerResult result =
        RuntimeOwnedFailure(rt, StrBuf_View(&error));
    ArenaRelease(&scratch);
    return result;
  }
  ArenaRelease(&scratch);
  return AppCommandHandlerResult{
      AppCommandHandlerStatus::Success,
      SVL("Trust decision saved. Restart Clank for it to take effect.")};
}

static AppCommandHandlerResult HandleReloadCommand(
    void* user, const AppCommandUx* ux, StrView) {
  InteractiveRuntime* rt = (InteractiveRuntime*)user;
  if (!rt->Harness || !rt->Prompt || rt->Harness->Running) {
    return AppCommandHandlerResult{
        AppCommandHandlerStatus::Failed,
        SVL("Prompt context can only be reloaded while the agent is idle.")};
  }

  Arena scratch = ArenaCreate((usize)16 << 20);
  DefaultProjectTrust defaultTrust = DefaultProjectTrust::Ask;
  AgentCompactionPolicy compaction = AgentCompactionPolicy_Default();
  AppSettings settings = {};
  AppSettingsLoadResult settingsResult =
      AppSettings_Load(&scratch, rt->SettingsPath, &settings);
  if (settingsResult == AppSettingsLoadResult::Success) {
    defaultTrust = settings.DefaultTrust;
    compaction = settings.Compaction;
  } else if (settingsResult == AppSettingsLoadResult::Malformed ||
             settingsResult == AppSettingsLoadResult::ReadFailure) {
    StrView message =
        settingsResult == AppSettingsLoadResult::Malformed
            ? SVL("Settings are malformed; prompt context was not reloaded.")
            : SVL("Settings could not be read; prompt context was not reloaded.");
    AppCommandHandlerResult result = RuntimeOwnedFailure(rt, message);
    ArenaRelease(&scratch);
    return result;
  }

  PromptContext next = {};
  bool hasProjectResources =
      ProjectTrust_HasGatedResources(&scratch, rt->WorkingDirectory);
  ProjectTrustReloadResolution trust = ProjectTrust_ResolveReload(
      hasProjectResources, rt->TrustCoversProjectResources,
      rt->TrustOverride, rt->ProjectTrusted);
  if (!trust.CanReload) {
    ArenaRelease(&scratch);
    return AppCommandHandlerResult{
        AppCommandHandlerStatus::Failed,
        SVL("New project prompt files were detected. Restart Clank to resolve trust.")};
  }
  PromptContextOptions options = rt->PromptOptions;
  options.ProjectTrusted = trust.Trusted;
  StrBuf error = StrBuf_Make(&scratch, 160);
  if (!PromptContext_Build(&options, &next, &error)) {
    AppCommandHandlerResult result =
        RuntimeOwnedFailure(rt, StrBuf_View(&error));
    ArenaRelease(&scratch);
    return result;
  }

  PromptContext previous = *rt->Prompt;
  *rt->Prompt = next;
  rt->Harness->Config.SystemPrompt = rt->Prompt->SystemPrompt;
  rt->Harness->Config.Compaction = compaction;
  rt->DefaultTrust = defaultTrust;
  rt->ProjectTrusted = trust.Trusted;
  rt->HasProjectResources = hasProjectResources;
  AppAuth_Invalidate(rt->Auth);
  PromptContext_Release(&previous);

  EmitPromptDiagnostics(rt->Prompt, ux);
  StrView report =
      PromptContextReport(rt->Mem, rt->Prompt, rt->HasProjectResources,
                          rt->ProjectTrusted, SVL("Prompt context reloaded."));
  ArenaRelease(&scratch);
  return AppCommandHandlerResult{AppCommandHandlerStatus::Success, report};
}

bool RuntimeResetSession(InteractiveRuntime* rt, const AppCommandUx* ux) {
  if (!rt->Harness || !rt->Sessions || rt->Harness->Running) {
    EmitCommandError(ux, SVL("Unable to clear the session while the agent is active."));
    return false;
  }

  SessionCreateOptions options = {};
  options.ProjectKey = rt->ProjectKey;
  options.WorkingDirectory = rt->WorkingDirectory;
  SessionLog log = {};
  SessionInfo info = {};
  SessionResult created =
      rt->Sessions->Create(rt->Sessions->Ctx, rt->Mem, &options, &log, &info);
  if (!SessionResult_IsOk(created)) {
    EmitCommandError(ux, created.Message);
    return false;
  }

  SessionResult switched =
      RuntimeSwitchToLog(rt, log, &info, nullptr, ux);
  if (!SessionResult_IsOk(switched)) {
    EmitCommandError(ux, switched.Message);
    return false;
  }
  SessionResult selected =
      AgentHarness_SetModel(rt->Harness, rt->CurrentModel,
                            rt->Harness->Config.ProviderImpl,
                            rt->Harness->Config.Stream.Profile);
  if (!SessionResult_IsOk(selected)) {
    EmitCommandError(ux, selected.Message);
    return false;
  }

  EmitCommandNotice(ux, SVL("session cleared"));
  return true;
}

AppCommandHandlers RuntimeCommandHandlers(InteractiveRuntime* rt) {
  AppCommandHandlers handlers = {};
  handlers.User = rt;
  handlers.Session = HandleSessionCommand;
  handlers.Name = HandleNameCommand;
  handlers.Sessions = HandleSessionsCommand;
  handlers.Resume = HandleResumeCommand;
  handlers.Tree = HandleTreeCommand;
  handlers.Checkout = HandleCheckoutCommand;
  handlers.Fork = HandleForkCommand;
  handlers.Compact = HandleCompactCommand;
  handlers.Reload = HandleReloadCommand;
  bool selectorsAvailable = rt->TuiView || Term_IsInteractive();
  if (selectorsAvailable) handlers.Trust = HandleTrustCommand;
  if (selectorsAvailable) {
    handlers.Login = HandleLoginCommand;
    handlers.Model = HandleModelCommand;
  }
  return handlers;
}




