#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent/event_json.h"
#include "agent/harness.h"
#include "app/auth_flow.h"
#include "app/command.h"
#include "app/commands.h"
#include "app/event_pump.h"
#include "app/hook_runner.h"
#include "app/plain_select.h"
#include "app/project_trust.h"
#include "app/prompt_context.h"
#include "app/provider_registry.h"
#include "app/runtime.h"
#include "app/session_switch.h"
#include "app/settings.h"
#include "auth/oauth.h"
#include "auth/resolve.h"
#include "auth/store.h"
#include "base/arena.h"
#include "base/str.h"
#include "base/sysinfo.h"
#include "base/thread.h"

#include "net/browser.h"
#include "net/http.h"
#include "term/term.h"
#include "tui/tui.h"
#include "term/sanitize.h"
#include "tools/tools.h"
#include "tools/fs.h"
#include "tools/filelock.h"
#include "session/jsonl_repository.h"
#include "session/memory_repository.h"

#if defined(_WIN32)
#include <windows.h>
static void EnableTerminal() {
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD m = 0;
  if (GetConsoleMode(h, &m)) SetConsoleMode(h, m | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
  SetConsoleOutputCP(CP_UTF8);
}
#else
static void EnableTerminal() {}
#endif

static void OnSigint(int) {
  Atomic_StoreI32(&g_abort, 1);
  if (g_activeHarness) Atomic_StoreI32(&g_activeHarness->AbortFlag, 1);
}

// ---- auth wiring ---------------------------------------------------------

struct HarnessEventPump;
struct PromptContextLifetime {
  PromptContext* Context;
  ~PromptContextLifetime() { PromptContext_Release(Context); }
};



// ---- REPL renderer -------------------------------------------------------







static void MaybeReportMem() {
  if (getenv("CLANK_PRINT_MEM")) {
    u64 b = PeakRssBytes();
    fprintf(stderr, "[peak RSS: %llu KB]\n", (unsigned long long)(b / 1024));
  }
}

// ---- command UX ----------------------------------------------------------



static void PrintHelp() {
  printf(
      "Clank - a tiny memory-efficient coding agent\n\n"
      "Usage: clank [options]\n"
      "  -m <model>        Override the configured model for this run only\n"
      "  -p <prompt>       Run a single prompt and exit (print mode)\n"
      "  --mode <text|json>\n"
      "                    Print-mode output: human text (default) or an\n"
      "                    NDJSON event stream on stdout. Requires -p\n"
      "  -c, --continue    Continue the most recent session for this project\n"
      "  -r, --resume ID   Resume an exact session ID from any project\n"
      "  --no-session      Keep the session in memory; write no session files\n"
      "  --system-prompt VALUE\n"
      "                    Replace the built-in prompt with text or file content\n"
      "  --append-system-prompt VALUE\n"
      "                    Append text or file content (repeatable)\n"
      "  -nc, --no-context-files\n"
      "                    Do not load AGENTS.md or CLAUDE.md files\n"
      "  -a, --approve     Trust project prompt resources for this run\n"
      "  -na, --no-approve Do not trust project prompt resources for this run\n"
      "  --login [prov]    OAuth login in your browser (anthropic or openai)\n"
      "  --logout [prov]   Remove stored credentials (default: anthropic)\n"
      "  --no-open         Do not auto-open a browser for --login or interactive /login\n"
     "  --help            Show this help\n\n"
      "Auth: run `--login anthropic` for Claude Pro/Max or `--login openai` for\n"
      "a ChatGPT subscription; alternatively set ANTHROPIC_API_KEY or OPENAI_API_KEY.\n"
      "Interactive commands: /session, /name, /sessions, /resume, /tree,\n"
      "/checkout, /fork, /compact, /trust, /reload, /login, /model,\n"
      "/clear, /exit.\n");
}


// Plain line REPL (non-TTY / fallback): the original fgets loop.
static void RunPlainRepl(InteractiveRuntime* rt) {
  EnableTerminal();
  rt->TuiView = nullptr;
  HarnessEventPump pump = {};
  AgentHarnessSnapshot snapshot = {};
  HarnessEventSink sink = {nullptr, ReplSink};
  SessionResult watching =
      StartHarnessEventPump(rt->Mem, rt->Harness, sink, nullptr, &pump,
                            &snapshot);
  if (!SessionResult_IsOk(watching)) {
    ReportHarnessResult(watching);
    return;
  }
  rt->EventPump = &pump;
  rt->EventSink = sink;
  printf("Clank (%.*s / %.*s). Ctrl+C interrupts a turn, /exit quits.\n",
           (int)rt->Spec->Name.Length, rt->Spec->Name.Data, (int)rt->CurrentModel.Id.Length,
           rt->CurrentModel.Id.Data);

  char line[8192];
  AppCommandUx commandUx = {nullptr, PlainCommandNotice, PlainCommandError};
  EmitCommandNotice(
      &commandUx,
      PromptContextReport(rt->Mem, rt->Prompt, rt->HasProjectResources,
                          rt->ProjectTrusted, SVL("Prompt context loaded.")));
  EmitPromptDiagnostics(rt->Prompt, &commandUx);
  RuntimeCheckAuth(rt, &commandUx, false);
  AppCommandHandlers handlers = RuntimeCommandHandlers(rt);
  for (;;) {
    fputs("\n\x1b[1m\xe2\x80\xba \x1b[0m", stdout);
    fflush(stdout);
    if (!fgets(line, sizeof(line), stdin)) break;
    size_t L = strlen(line);
    while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = 0;
    if (L == 0) continue;
    AppCommandDispatchResult command = AppCommand_Dispatch(SV(line), &handlers, &commandUx);
    if (command.Handled) {
      if (command.Action == AppLoopAction::Exit) break;
      if (command.Action == AppLoopAction::ResetSession) RuntimeResetSession(rt, &commandUx);
      continue;
    }
    Atomic_StoreI32(&g_abort, 0);
    if (!RuntimeCheckAuth(rt, &commandUx, true)) continue;
    if (Atomic_LoadI32(&g_abort)) continue;
   SessionResult run = RunHarnessPrompt(rt->Harness, SV(line));
    ReportHarnessResult(run);
  }
  StopHarnessEventPump(&pump);
  rt->EventPump = nullptr;
}

// Full-screen TUI REPL. Falls back to the plain REPL if not a TTY.
static void RunTuiRepl(InteractiveRuntime* rt) {
  TuiOpts opts;
  opts.ModelName = rt->CurrentModel.Id;
  opts.Profile = rt->Harness->Config.Stream.Profile;
  opts.Color = (getenv("NO_COLOR") == nullptr);
  u32 catalogCount = AppCommandSpec_Count();
  TuiCommandSpec* commands = ArenaPushArray(rt->Mem, TuiCommandSpec, catalogCount);
  for (u32 i = 0; i < catalogCount; i++) {
    const AppCommandSpec* command = AppCommandSpec_At(i);
    commands[opts.CommandCount++] =
        TuiCommandSpec{command->Name, command->Alias, command->Description};
  }
  opts.Commands = commands;

  Tui tui;
  if (!Tui_Start(&tui, rt->Mem, opts)) {
    Http_GlobalInit();
    RunPlainRepl(rt);
    return;
  }
  rt->TuiView = &tui;
  tui.Harness = rt->Harness;

  HarnessEventPump pump = {};
  AgentHarnessSnapshot snapshot = {};
  HarnessEventSink sink = {&tui, Tui_HarnessEvent};
  SessionResult watching =
      StartHarnessEventPump(rt->Mem, rt->Harness, sink, &tui, &pump,
                            &snapshot);
  if (!SessionResult_IsOk(watching)) {
    ReportHarnessResult(watching);
    Tui_Stop(&tui);
    rt->TuiView = nullptr;
    return;
  }
  rt->EventPump = &pump;
  rt->EventSink = sink;
  Tui_HydrateTranscript(&tui, snapshot.Transcript.Data,
                        snapshot.Transcript.Count);

  AuthWarmup warmup = {};
  Http_GlobalInit();
  rt->AuthWarmupState = &warmup;
  StartAuthWarmup(&warmup, rt->Auth, rt->Spec, Tui_OperationAbort(&tui));
  Tui_LoadStartupContext(&tui);

  AppCommandUx commandUx = {&tui, TuiCommandNotice, TuiCommandError};
  EmitCommandNotice(
      &commandUx,
      PromptContextReport(rt->Mem, rt->Prompt, rt->HasProjectResources,
                          rt->ProjectTrusted, SVL("Prompt context loaded.")));
  EmitPromptDiagnostics(rt->Prompt, &commandUx);
  AppCommandHandlers handlers = RuntimeCommandHandlers(rt);
  for (;;) {
    TuiSubmission submission = {};
    if (!Tui_WaitForSubmit(&tui, &submission)) break;
    if (submission.Kind == TuiSubmissionKind::Command) {
      AppCommandDispatchResult command =
          AppCommand_Dispatch(submission.Text, &handlers, &commandUx);
      if (command.Action == AppLoopAction::Exit) break;
      if (command.Action == AppLoopAction::ResetSession) RuntimeResetSession(rt, &commandUx);
      continue;
    }
    Tui_BeginOperation(&tui, SVL("Resolving credentials"));
    bool authenticated = RuntimeCheckAuth(rt, &commandUx, true);
    bool cancelled = Tui_EndOperation(&tui);
    if (!authenticated || cancelled) continue;
    *Tui_Abort(&tui) = 0;
    if (!Tui_AcceptPrompt(&tui, submission.Text)) continue;
   SessionResult run = RunHarnessPrompt(rt->Harness, submission.Text);
    if (!SessionResult_IsOk(run)) Tui_CommandError(&tui, run.Message);
  }
  CancelRuntimeAuthWarmup(rt);
  StopHarnessEventPump(&pump);
  rt->EventPump = nullptr;
  Tui_Stop(&tui);
  rt->TuiView = nullptr;
}

int main(int argc, char** argv) {
  Arena arena = ArenaCreate((usize)2 << 30);
  const char* modelOverride = nullptr;
  const char* onePrompt = nullptr;
  bool jsonMode = false;
  const char* loginProvider = nullptr;
  const char* logoutProvider = nullptr;
  const char* resumeSession = nullptr;
  bool hasExplicitSystemPrompt = false;
  StrView explicitSystemPrompt = {};
  Array<StrView> explicitAppendPrompts = Array_Make<StrView>(&arena, 4);
  ProjectTrustOverride trustOverride = ProjectTrustOverride::Unspecified;
  bool noOpen = false;
  bool noSession = false;
  bool noContextFiles = false;
  bool continueSession = false;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-m") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: -m requires a model\n");
        ArenaRelease(&arena);
        return 1;
      }
      modelOverride = argv[++i];
    } else if (strcmp(argv[i], "-p") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: -p requires a prompt\n");
        ArenaRelease(&arena);
        return 1;
      }
      onePrompt = argv[++i];
    } else if (strcmp(argv[i], "--mode") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: --mode requires text or json\n");
        ArenaRelease(&arena);
        return 1;
      }
      const char* value = argv[++i];
      if (strcmp(value, "json") == 0) {
        jsonMode = true;
      } else if (strcmp(value, "text") != 0) {
        fprintf(stderr, "error: unknown mode '%s' (expected text or json)\n", value);
        ArenaRelease(&arena);
        return 1;
      }
    } else if (strcmp(argv[i], "--system-prompt") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: --system-prompt requires a value\n");
        ArenaRelease(&arena);
        return 1;
      }
      hasExplicitSystemPrompt = true;
      explicitSystemPrompt = SV(argv[++i]);
    } else if (strcmp(argv[i], "--append-system-prompt") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: --append-system-prompt requires a value\n");
        ArenaRelease(&arena);
        return 1;
      }
      Array_Push(&explicitAppendPrompts, SV(argv[++i]));
    } else if (strcmp(argv[i], "-nc") == 0 ||
               strcmp(argv[i], "--no-context-files") == 0) {
      noContextFiles = true;
    } else if (strcmp(argv[i], "-a") == 0 ||
               strcmp(argv[i], "--approve") == 0) {
      if (trustOverride == ProjectTrustOverride::Untrusted) {
        fprintf(stderr,
                "error: choose only one of --approve or --no-approve\n");
        ArenaRelease(&arena);
        return 1;
      }
      trustOverride = ProjectTrustOverride::Trusted;
    } else if (strcmp(argv[i], "-na") == 0 ||
               strcmp(argv[i], "--no-approve") == 0) {
      if (trustOverride == ProjectTrustOverride::Trusted) {
        fprintf(stderr,
                "error: choose only one of --approve or --no-approve\n");
        ArenaRelease(&arena);
        return 1;
      }
      trustOverride = ProjectTrustOverride::Untrusted;
    } else if (strcmp(argv[i], "--no-session") == 0) {
      noSession = true;
    } else if (strcmp(argv[i], "-c") == 0 ||
               strcmp(argv[i], "--continue") == 0) {
      continueSession = true;
    } else if ((strcmp(argv[i], "-r") == 0 ||
                strcmp(argv[i], "--resume") == 0)) {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: --resume requires a session ID\n");
        ArenaRelease(&arena);
        return 1;
      }
      resumeSession = argv[++i];
    } else if (strcmp(argv[i], "--no-open") == 0) {
      noOpen = true;
    } else if (strcmp(argv[i], "--login") == 0) {
      loginProvider = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : "anthropic";
    } else if (strcmp(argv[i], "--logout") == 0) {
      logoutProvider = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : "anthropic";
    } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      PrintHelp();
      ArenaRelease(&arena);
      return 0;
    } else {
      fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
      ArenaRelease(&arena);
      return 1;
    }
  }

  if ((continueSession && resumeSession) ||
      (noSession && (continueSession || resumeSession))) {
    fprintf(stderr,
            "error: choose only one of --no-session, --continue, or --resume\n");
    ArenaRelease(&arena);
    return 1;
  }

  // JSON is a machine surface for a single non-interactive run; there is no
  // meaningful interactive form of it without a bidirectional protocol.
  if (jsonMode && !onePrompt) {
    fprintf(stderr, "error: --mode json requires -p <prompt>\n");
    ArenaRelease(&arena);
    return 1;
  }

  StrView authPath = AuthStore_DefaultPath(&arena);
  StrView settingsPath = AppSettings_DefaultPath(&arena);
  signal(SIGINT, OnSigint);

  if (loginProvider) {
    Http_GlobalInit();
    int rc = RunCommandLineLogin(&arena, authPath, loginProvider, !noOpen);
    Http_GlobalShutdown();
    ArenaRelease(&arena);
    return rc;
  }
  if (logoutProvider) {
    int rc = RunCommandLineLogout(&arena, authPath, logoutProvider);
    Http_GlobalShutdown();
    ArenaRelease(&arena);
    return rc;
  }

  AppAuth appAuth = {};
  appAuth.Mem = &arena;
  appAuth.Path = authPath;
  RuntimeSelection selected =
      LoadRuntimeSelection(&arena, settingsPath, modelOverride);
  if (!selected.Spec || !selected.Spec->CreateProvider || !selected.Spec->CreateModel) {
    fprintf(stderr, "error: no usable built-in provider is registered\n");
    Http_GlobalShutdown();
    ArenaRelease(&arena);
    return 1;
  }

  StrView workingDirectory = Sys_CurrentDir(&arena);
  if (!workingDirectory.Length) {
    fprintf(stderr, "error: could not determine the current working directory\n");
    ArenaRelease(&arena);
    return 1;
  }
  StrView projectKey = SessionProjectKey(&arena, workingDirectory);
  MemoryRepository memoryRepository = {};
  JsonlRepository jsonlRepository = {};
  SessionRepository sessions = {};
  if (noSession) {
    MemoryRepository_Init(&memoryRepository);
    sessions = MemoryRepository_AsRepository(&memoryRepository);
  } else {
    StrView dataRoot = SessionDataRoot(&arena);
    JsonlRepository_Init(&jsonlRepository, dataRoot);
    sessions = JsonlRepository_AsRepository(&jsonlRepository);
  }

  SessionInfo resolvedResume = {};
  DurableId resolvedResumeId = {};
  if (resumeSession) {
    if (!DurableId_Parse(SV(resumeSession), &resolvedResumeId)) {
      fprintf(stderr, "error: invalid session ID\n");
      sessions.Close(sessions.Ctx);
      ArenaRelease(&arena);
      return 1;
    }
    SessionResult resolved = sessions.Resolve(
        sessions.Ctx, &arena, resolvedResumeId, &resolvedResume);
    if (!SessionResult_IsOk(resolved)) {
      fprintf(stderr, "error: session startup failed: %.*s\n",
              (int)resolved.Message.Length, resolved.Message.Data);
      sessions.Close(sessions.Ctx);
      ArenaRelease(&arena);
      return 1;
    }
    if (!Fs_IsDir(resolvedResume.WorkingDirectory)) {
      fprintf(stderr,
              "error: session working directory no longer exists: %.*s\n",
              (int)resolvedResume.WorkingDirectory.Length,
              resolvedResume.WorkingDirectory.Data);
      sessions.Close(sessions.Ctx);
      ArenaRelease(&arena);
      return 1;
    }
    if (!Sys_SetCurrentDir(resolvedResume.WorkingDirectory)) {
      fprintf(stderr, "error: could not enter session working directory: %.*s\n",
              (int)resolvedResume.WorkingDirectory.Length,
              resolvedResume.WorkingDirectory.Data);
      sessions.Close(sessions.Ctx);
      ArenaRelease(&arena);
      return 1;
    }
    workingDirectory =
        StrView_Dup(&arena, resolvedResume.WorkingDirectory);
    projectKey = StrView_Dup(&arena, resolvedResume.ProjectKey);
  }
  StrView agentDirectory = Fs_Parent(settingsPath);
  StrView trustPath = ProjectTrust_DefaultPath(&arena);
  bool projectTrusted = false;
  bool hasProjectResources = false;
  bool trustInteractive = !onePrompt && Term_IsInteractive();
  if (!ResolveStartupProjectTrust(
          &arena, trustPath, workingDirectory, selected.DefaultTrust,
          trustOverride, trustInteractive, &projectTrusted,
          &hasProjectResources)) {
    sessions.Close(sessions.Ctx);
    ArenaRelease(&arena);
    return 1;
  }

  Model model = selected.Spec->CreateModel(&arena, selected.ModelId);
  bool unsupportedProfilePreference = false;
  ModelRequestProfile preference = Model_NormalizeRequestPreference(
      &model, selected.RequestedProfile, &unsupportedProfilePreference);
  ModelRequestProfile profile = Model_ResolveRequestProfile(&model, preference);
  if (unsupportedProfilePreference) {
    fprintf(stderr,
            "warning: saved effort/context preferences are unsupported by model "
            "'%.*s'; using model defaults where needed\n",
            (int)model.Id.Length, model.Id.Data);
  }
  FileLock_Init();
  // Repointed whenever the durable session changes, so shell overflow logs
  // always land beside the log they belong to. Stays empty under
  // `--no-session`, which falls the tool back to the OS temp directory.
  ShellToolContext shellContext = {};
  Tool tools[] = {ReadTool(), WriteTool(), EditTool(),        LsTool(),
                  FindTool(), GrepTool(), ShellTool(&shellContext)};

  PromptContextOptions promptOptions = {};
  promptOptions.WorkingDirectory = workingDirectory;
  promptOptions.AgentDirectory = agentDirectory;
  promptOptions.Tools = tools;
  promptOptions.ToolCount = sizeof(tools) / sizeof(tools[0]);
  promptOptions.NoContextFiles = noContextFiles;
  promptOptions.ProjectTrusted = projectTrusted;
  promptOptions.HasExplicitSystemPrompt = hasExplicitSystemPrompt;
  promptOptions.ExplicitSystemPrompt = explicitSystemPrompt;
  promptOptions.ExplicitAppendPrompts = explicitAppendPrompts.Data;
  promptOptions.ExplicitAppendPromptCount = explicitAppendPrompts.Count;
  PromptContext promptContext = {};
  StrBuf promptError = StrBuf_Make(&arena, 160);
  if (!PromptContext_Build(&promptOptions, &promptContext, &promptError)) {
    fprintf(stderr, "error: %.*s\n", (int)promptError.Length,
            promptError.Data);
    sessions.Close(sessions.Ctx);
    ArenaRelease(&arena);
    return 1;
  }
  PromptContextLifetime promptLifetime = {&promptContext};
  if (onePrompt) {
    if (hasProjectResources && !projectTrusted)
      fprintf(stderr,
              "warning: project .clank prompt files are disabled because the project is untrusted\n");
    PrintPromptDiagnostics(&promptContext);
  }

  AgentHarnessConfig harnessConfig = {};
  harnessConfig.SystemPrompt = promptContext.SystemPrompt;
  harnessConfig.ModelInfo = model;
  harnessConfig.Tools = tools;
  harnessConfig.ToolCount = sizeof(tools) / sizeof(tools[0]);
  harnessConfig.ProviderImpl = selected.Spec->CreateProvider(&arena);
  harnessConfig.Stream.Profile = profile;
  harnessConfig.ToolExec = ToolExecution::Parallel;
  harnessConfig.Compaction = selected.Compaction;
  harnessConfig.AuthUser = &appAuth;
  harnessConfig.ResolveAuth = AppResolveAuth;

  SessionLog sessionLog = {};
  SessionInfo sessionInfo = {};
  SessionResult sessionResult = {};
  bool openedExisting = continueSession || resumeSession;
  if (continueSession) {
    Array<SessionInfo> items = {};
    SessionListOptions list = {};
    list.Scope = SessionListScope::CurrentProject;
    list.ProjectKey = projectKey;
    u32 skippedCount = 0;
    list.SkippedCount = &skippedCount;
    sessionResult =
        sessions.List(sessions.Ctx, &arena, &list, &items);
    if (SessionResult_IsOk(sessionResult) && skippedCount)
      fprintf(stderr, "warning: skipped %u unreadable session file%s\n",
              skippedCount, skippedCount == 1 ? "" : "s");
    if (SessionResult_IsOk(sessionResult) && items.Count == 0)
      sessionResult = SessionResult_Error(
          SessionStatus::NotFound,
          SVL("no previous session exists for this project"));
    if (SessionResult_IsOk(sessionResult)) {
      SessionOpenOptions open = {};
      open.SessionId = items.Data[0].SessionId;
      open.ProjectKey = projectKey;
      open.Writable = true;
      sessionResult =
          sessions.Open(sessions.Ctx, &arena, &open, &sessionLog, &sessionInfo);
    }
  } else if (resumeSession) {
    SessionOpenOptions open = {};
    open.SessionId = resolvedResumeId;
    open.ProjectKey = resolvedResume.ProjectKey;
    open.Writable = true;
    sessionResult =
        sessions.Open(sessions.Ctx, &arena, &open, &sessionLog, &sessionInfo);
  } else {
    SessionCreateOptions create = {};
    create.ProjectKey = projectKey;
    create.WorkingDirectory = workingDirectory;
    sessionResult =
        sessions.Create(sessions.Ctx, &arena, &create, &sessionLog, &sessionInfo);
  }
  if (!SessionResult_IsOk(sessionResult)) {
    fprintf(stderr, "error: session startup failed: %.*s\n",
            (int)sessionResult.Message.Length, sessionResult.Message.Data);
    sessions.Close(sessions.Ctx);
    Http_GlobalShutdown();
    ArenaRelease(&arena);
    return 1;
  }
  if (!noSession) {
    shellContext.Directory = JsonlRepository_SessionDir(
        &arena, &jsonlRepository, sessionInfo.ProjectKey, sessionInfo.SessionId);
  }

  AgentHarness harness = {};
  SessionResult initialized =
      AgentHarness_Init(&harness, sessionLog, &harnessConfig);
  if (!SessionResult_IsOk(initialized)) {
    fprintf(stderr, "error: session restore failed: %.*s\n",
            (int)initialized.Message.Length, initialized.Message.Data);
    sessions.Close(sessions.Ctx);
    Http_GlobalShutdown();
    ArenaRelease(&arena);
    return 1;
  }

  if (openedExisting) {
    Arena scratch = ArenaCreate((usize)16 << 20);
    SessionReduction state = {};
    SessionResult snapshot =
        AgentHarness_Snapshot(&scratch, &harness, &state);
    if (SessionResult_IsOk(snapshot) && state.Provider.Length &&
        state.Model.Length) {
      {
        const ProviderSpec* restored = ProviderRegistry_Find(state.Provider);
        if (!restored || !restored->CreateProvider ||
            !restored->CreateModel) {
          fprintf(stderr,
                  "error: session uses unavailable provider '%.*s'\n",
                  (int)state.Provider.Length, state.Provider.Data);
          ArenaRelease(&scratch);
          AgentHarness_Close(&harness);
          sessions.Close(sessions.Ctx);
          ArenaRelease(&arena);
          return 1;
        }
        selected.Spec = restored;
        model = restored->CreateModel(
            &arena, modelOverride ? SV(modelOverride) : state.Model);
        profile = Model_ResolveRequestPreference(&model, preference);
        harness.Config.ModelInfo = model;
        harness.Config.ProviderImpl = restored->CreateProvider(&arena);
        harness.Config.Stream.Profile = profile;
        if (modelOverride) {
          SessionResult overridden = AgentHarness_SetModel(
              &harness, model, harness.Config.ProviderImpl, profile);
          if (!SessionResult_IsOk(overridden)) {
            fprintf(stderr, "error: failed to apply model override: %.*s\n",
                    (int)overridden.Message.Length, overridden.Message.Data);
            ArenaRelease(&scratch);
            AgentHarness_Close(&harness);
            sessions.Close(sessions.Ctx);
            ArenaRelease(&arena);
            return 1;
          }
        }
      }
    } else if (SessionResult_IsOk(snapshot)) {
      SessionResult selectedModel = AgentHarness_SetModel(
          &harness, model, harness.Config.ProviderImpl, profile);
      if (!SessionResult_IsOk(selectedModel)) {
        fprintf(stderr, "error: failed to initialize session model: %.*s\n",
                (int)selectedModel.Message.Length,
                selectedModel.Message.Data);
        ArenaRelease(&scratch);
        AgentHarness_Close(&harness);
        sessions.Close(sessions.Ctx);
        ArenaRelease(&arena);
        return 1;
      }
    }
    ArenaRelease(&scratch);
  } else {
    SessionResult selectedModel = AgentHarness_SetModel(
        &harness, model, harnessConfig.ProviderImpl, profile);
    if (!SessionResult_IsOk(selectedModel)) {
      fprintf(stderr, "error: failed to persist initial model: %.*s\n",
              (int)selectedModel.Message.Length, selectedModel.Message.Data);
      AgentHarness_Close(&harness);
      sessions.Close(sessions.Ctx);
      ArenaRelease(&arena);
      return 1;
    }
  }

  InteractiveRuntime runtime = {};
  runtime.Mem = &arena;
  runtime.AuthPath = authPath;
  runtime.SettingsPath = settingsPath;
  runtime.Spec = selected.Spec;
  runtime.CurrentModel = model;
  runtime.PreferredProfile = preference;
  runtime.Harness = &harness;
  runtime.Sessions = &sessions;
  runtime.JsonlSessions = noSession ? nullptr : &jsonlRepository;
  runtime.Shell = &shellContext;
  runtime.ProjectKey = projectKey;
  runtime.WorkingDirectory = workingDirectory;
  runtime.AgentDirectory = agentDirectory;
  runtime.TrustPath = trustPath;
  runtime.NoSession = noSession;
  runtime.ProjectTrusted = projectTrusted;
  runtime.HasProjectResources = hasProjectResources;
  runtime.TrustCoversProjectResources =
      hasProjectResources ||
      trustOverride != ProjectTrustOverride::Unspecified;
  runtime.TrustOverride = trustOverride;
  runtime.DefaultTrust = selected.DefaultTrust;
  runtime.Prompt = &promptContext;
  runtime.PromptOptions = promptOptions;
  runtime.AutoOpen = !noOpen;
  runtime.Auth = &appAuth;
  // External lifecycle hooks. Started only once the session is durable, so the
  // very first payload can already name the transcript a consumer should read.
  // `--no-session` has no durable path to report, so it reports nothing at all.
  HookConfig hookConfig = {};
  if (!noSession) {
    StrView hooksPath = HookConfig_DefaultPath(&arena);
    HookConfigLoadResult hooksLoad = HookConfig_Load(&arena, hooksPath, &hookConfig);
    // Surfaced the way a malformed settings.json is: a typo must not disable
    // every hook in silence. Missing is the normal case and stays quiet.
    if (hooksLoad == HookConfigLoadResult::Malformed ||
        hooksLoad == HookConfigLoadResult::ReadFailure) {
      fprintf(stderr, "warning: %s %.*s; hooks are disabled\n",
              hooksLoad == HookConfigLoadResult::Malformed
                  ? "hooks file is malformed:"
                  : "could not read hooks file:",
              (int)hooksPath.Length, hooksPath.Data);
    }

    HookSessionInfo hookSession = {};
    hookSession.SessionId = DurableId_View(&sessionInfo.SessionId);
    hookSession.WorkingDirectory = workingDirectory;
    hookSession.TranscriptPath = JsonlRepository_SessionPath(
        &arena, &jsonlRepository, sessionInfo.ProjectKey, sessionInfo.SessionId);
    g_hooks = HookRunner_Create(&arena, &hookConfig, &hookSession);
    HookRunner_Dispatch(g_hooks, HookEvent::SessionStart, StrView{});
  }

  if (onePrompt) {
    EnableTerminal();
    Http_GlobalInit();
    HarnessEventPump pump = {};
    AgentHarnessSnapshot snapshot = {};
    HarnessEventSink sink = {nullptr, ReplSink};
    SessionResult watching =
        StartHarnessEventPump(&arena, &harness, sink, nullptr, &pump,
                              &snapshot, jsonMode);
    if (!SessionResult_IsOk(watching)) {
      ReportHarnessResult(watching);
      EndHookSession();
      AgentHarness_Close(&harness);
      sessions.Close(sessions.Ctx);
      Http_GlobalShutdown();
      ArenaRelease(&arena);
      return 1;
    }
    Atomic_StoreI32(&g_abort, 0);
    if (!ResolvePromptAuth(&harness, &g_abort)) {
      if (Atomic_LoadI32(&g_abort)) {
        StopHarnessEventPump(&pump);
        EndHookSession();
        AgentHarness_Close(&harness);
        sessions.Close(sessions.Ctx);
        Http_GlobalShutdown();
        ArenaRelease(&arena);
        return 130;
      }
      StrView authGuidance = AuthGuidance(&arena, runtime.Spec, false);
      fprintf(stderr, "error: %.*s\n", (int)authGuidance.Length, authGuidance.Data);
      StopHarnessEventPump(&pump);
      EndHookSession();
      AgentHarness_Close(&harness);
      sessions.Close(sessions.Ctx);
      Http_GlobalShutdown();
      ArenaRelease(&arena);
      return 1;
    }
    Atomic_StoreI32(&g_abort, 0);
    SessionResult run = RunHarnessPrompt(&harness, SV(onePrompt));
    StopHarnessEventPump(&pump);
    if (jsonMode) {
      // Terminator: the pump has drained, so this is always the last line and
      // carries the same verdict as the exit code.
      StrBuf line = StrBuf_Make(&arena, 128);
      JsonWriter w = {};
      JsonWrite_Begin(&w, &line);
      JsonWrite_String(&w, "type", SVL("result"));
      JsonWrite_Bool(&w, "ok", SessionResult_IsOk(run));
      if (!SessionResult_IsOk(run)) JsonWrite_String(&w, "error", run.Message);
      JsonWrite_End(&w);
      fwrite(line.Data, 1, line.Length, stdout);
      fputc('\n', stdout);
      fflush(stdout);
    }
    ReportHarnessResult(run);
    MaybeReportMem();
    EndHookSession();
    AgentHarness_Close(&harness);
    sessions.Close(sessions.Ctx);
    Http_GlobalShutdown();
    ArenaRelease(&arena);
    return SessionResult_IsOk(run) ? 0 : 1;
  }

  if (Term_IsInteractive())
    RunTuiRepl(&runtime);
  else {
    Http_GlobalInit();
    RunPlainRepl(&runtime);
  }

  MaybeReportMem();
  EndHookSession();
  AgentHarness_Close(&harness);
  sessions.Close(sessions.Ctx);
  Http_GlobalShutdown();
  ArenaRelease(&arena);
  return 0;
}
