// Application-layer logic that used to be unreachable from tests.
//
// Everything here lived inside main.cpp, which has no library target, so none
// of it could be exercised. The behaviour is unchanged; only its address is.
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "app/config_paths.h"
#include "app/auth_flow.h"
#include "app/commands.h"
#include "app/event_pump.h"
#include "app/provider_registry.h"
#include "app/runtime.h"
#include "app/session_switch.h"
#include "app/settings.h"
#include "app/tui_bridge.h"
#include "base/arena.h"
#include "base/config_file.h"
#include "check.h"
#include "json/json.h"
#include "script_provider.h"
#include "session/metadata.h"
#include "session/memory_repository.h"

namespace {

void TestSessionProjectKey() {
  Arena arena = ArenaCreate();

  // The key is a filesystem-safe basename plus a hash of the full path, so two
  // directories sharing a basename never collide into one session directory.
  StrView a = SessionProjectKey(&arena, SVL("/home/dev/clanker"));
  StrView b = SessionProjectKey(&arena, SVL("/srv/build/clanker"));
  CHECK_ALL(!StrView_Eq(a, b), StrView_Contains(a, "clanker"),
            StrView_Contains(b, "clanker"));

  // Same input, same key -- session directories must be stable across runs.
  CHECK(StrView_Eq(a, SessionProjectKey(&arena, SVL("/home/dev/clanker"))));

  // Characters that cannot appear in a path component are replaced rather than
  // escaped, and the hash still separates the results.
  StrView odd = SessionProjectKey(&arena, SVL("/tmp/we ird:name*"));
  for (u32 i = 0; i < odd.Length; i++) {
    char c = odd.Data[i];
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    CHECK(ok);
  }

  // A trailing separator leaves no basename; the key still has to be usable.
  StrView rooted = SessionProjectKey(&arena, SVL("/"));
  CHECK(rooted.Length > 0);

#if defined(_WIN32)
  // Windows paths are compared case-insensitively with normalized separators,
  // so one directory spelled two ways resolves to a single session rather than
  // silently forking into two session directories.
  CHECK(StrView_Eq(SessionProjectKey(&arena, SVL("C:\\Work\\Clanker")),
                   SessionProjectKey(&arena, SVL("c:/work/clanker"))));
  // Distinct directories that differ only by case of a *parent* component still
  // agree, because the whole path feeds the hash.
  CHECK(StrView_Eq(SessionProjectKey(&arena, SVL("C:\\A\\proj")),
                   SessionProjectKey(&arena, SVL("c:\\a\\proj"))));
  // But genuinely different directories must not collide.
  CHECK(!StrView_Eq(SessionProjectKey(&arena, SVL("C:\\a\\proj")),
                    SessionProjectKey(&arena, SVL("C:\\b\\proj"))));
#endif

  ArenaRelease(&arena);
}

// A key recorded before the basename was case-folded still has to open, since
// on Windows it names the very same storage directory.
void TestProjectKeyComparison() {
  CHECK(SessionProjectKey_Eq(SVL("clanker-abc"), SVL("clanker-abc")));
  CHECK(!SessionProjectKey_Eq(SVL("clanker-abc"), SVL("clanker-abd")));
  CHECK(!SessionProjectKey_Eq(SVL("clanker-abc"), SVL("clanker-ab")));
#if defined(_WIN32)
  CHECK(SessionProjectKey_Eq(SVL("Clanker-abc"), SVL("clanker-abc")));
  CHECK(SessionProjectKey_Eq(SVL("MyProj-0011"), SVL("myproj-0011")));
#else
  CHECK(!SessionProjectKey_Eq(SVL("Clanker-abc"), SVL("clanker-abc")));
#endif
}

void TestConfigPaths() {  Arena arena = ArenaCreate();

  // Every per-user file hangs off one root, and the root is a prefix of each.
  StrView root = ClankerDataRoot(&arena);
  StrView settings = ClankerDataPath(&arena, "settings.json");
  CHECK_ALL(StrView_Contains(root, ".clanker"),
            StrView_Contains(settings, ".clanker/settings.json"),
            settings.Length > root.Length,
            memcmp(settings.Data, root.Data, root.Length) == 0);

  // An empty name is the root itself rather than a trailing slash.
  CHECK(StrView_Eq(ClankerDataRoot(&arena), ClankerDataPath(&arena, nullptr)));

  ArenaRelease(&arena);
}

void TestTrustChoiceMenu() {
  Arena arena = ArenaCreate();
  StrView options[TRUST_CHOICE_CAPACITY] = {};
  TrustChoice choices[TRUST_CHOICE_CAPACITY] = {};

  // `/trust` exists to persist a decision, so it drops the session-only
  // entries the startup prompt offers.
  u32 full = BuildTrustChoices(&arena, SVL("/home/dev"), true, options, choices);
  u32 persistOnly =
      BuildTrustChoices(&arena, SVL("/home/dev"), false, options, choices);
  CHECK_ALL(full <= TRUST_CHOICE_CAPACITY, persistOnly < full);

  for (u32 i = 0; i < persistOnly; i++)
    CHECK(choices[i] != TrustChoice::TrustSession &&
          choices[i] != TrustChoice::UntrustSession);

  // Without a parent directory there is no parent-scoped choice to offer.
  u32 noParent = BuildTrustChoices(&arena, StrView{}, true, options, choices);
  CHECK(noParent < full);

  // Trusting the parent supersedes any saved entry for the child, so the child
  // path is removed rather than left to shadow the new decision.
  TrustDecision parent =
      ResolveTrustChoice(TrustChoice::TrustParent, SVL("/home/dev/app"), SVL("/home/dev"));
  CHECK_ALL(parent.Trusted, parent.Persist,
            StrView_EqCStr(parent.DecisionPath, "/home/dev"),
            StrView_EqCStr(parent.RemovePath, "/home/dev/app"));

  // Session-only choices never touch the store.
  TrustDecision session =
      ResolveTrustChoice(TrustChoice::TrustSession, SVL("/home/dev/app"), SVL("/home/dev"));
  CHECK_ALL(session.Trusted, !session.Persist);

  TrustDecision untrustSession =
      ResolveTrustChoice(TrustChoice::UntrustSession, SVL("/home/dev/app"), SVL("/home/dev"));
  CHECK_ALL(!untrustSession.Trusted, !untrustSession.Persist);

  // Untrusting the current folder is a persisted decision about that folder.
  TrustDecision untrust =
      ResolveTrustChoice(TrustChoice::UntrustCurrent, SVL("/home/dev/app"), SVL("/home/dev"));
  CHECK_ALL(!untrust.Trusted, untrust.Persist,
            StrView_EqCStr(untrust.DecisionPath, "/home/dev/app"));

  ArenaRelease(&arena);
}

void TestCommandResultMapping() {
  // A failing SessionResult reports its own message and never the caller's
  // success text -- that mix-up would announce success on a failed command.
  AppCommandHandlerResult ok = CommandResult(SessionResult_Ok(), SVL("done"));
  CHECK_ALL(ok.Status == AppCommandHandlerStatus::Success,
            StrView_EqCStr(ok.Message, "done"));

  AppCommandHandlerResult failed =
      CommandResult(SessionResult_Invalid("bad input"), SVL("done"));
  CHECK_ALL(failed.Status == AppCommandHandlerStatus::Failed,
            StrView_EqCStr(failed.Message, "bad input"));
}

struct RuntimeCommandCapture {
  u32 Notices;
  u32 Errors;
  StrView LastNotice;
  StrView LastError;
};

void CaptureRuntimeNotice(void* user, StrView message) {
  RuntimeCommandCapture* capture = (RuntimeCommandCapture*)user;
  capture->Notices++;
  capture->LastNotice = message;
}

void CaptureRuntimeError(void* user, StrView message) {
  RuntimeCommandCapture* capture = (RuntimeCommandCapture*)user;
  capture->Errors++;
  capture->LastError = message;
}

Model TestReviewerModel(Arena* arena, const ProviderSpec* spec,
                        StrView modelId) {
  Model model = {};
  model.Id = StrView_Dup(arena, modelId);
  model.Name = StrView_Dup(arena, modelId);
  model.Provider = spec ? StrView_Dup(arena, spec->Id) : StrView{};
  return model;
}

}  // namespace

// Command results allocate from CommandMem, which a zero-initialized runtime
// does not have. A hand-built runtime that skips InteractiveRuntime_Init traps
// in ArenaAlloc the first time a handler formats a message.
void TestRuntimeCommandArena() {
  Arena arena = ArenaCreate();
  InteractiveRuntime rt = {};
  InteractiveRuntime_Init(&rt, &arena);
  CHECK_ALL(rt.Mem == &arena, rt.CommandMem.Reserved > 0);

  AppCommandHandlerResult failure = RuntimeOwnedFailure(&rt, SVL("boom"));
  CHECK_ALL(failure.Status == AppCommandHandlerStatus::Failed,
            StrView_EqCStr(failure.Message, "boom"));
  // The message is owned by CommandMem, not by the caller's string.
  CHECK(rt.CommandMem.Used > 0);

  // Dispatching recycles it, which is what keeps repeated commands from
  // accumulating in a process-lifetime arena.
  AppCommandHandlers handlers = {};
  handlers.User = &rt;
  AppCommandUx ux = {};
  usize afterFirst = rt.CommandMem.Used;
  RuntimeDispatchCommand(&rt, SVL("not a command"), &handlers, &ux);
  CHECK_ALL(afterFirst > 0, rt.CommandMem.Used == 0);

  InteractiveRuntime_Release(&rt);
  CHECK(rt.CommandMem.Base == nullptr);
  ArenaRelease(&arena);
}

void TestRuntimeSetPromptOptions() {
  Arena arena = ArenaCreate();
  InteractiveRuntime rt = {};
  InteractiveRuntime_Init(&rt, &arena);

  StrView settingsSkills[] = {SVL("/skills/a")};
  StrView settingsTemplates[] = {SVL("/prompts/a"), SVL("/prompts/b")};
  PromptContext context = {};
  context.SettingsSkillPaths.Data = settingsSkills;
  context.SettingsSkillPaths.Count = 1;
  context.SettingsPromptTemplatePaths.Data = settingsTemplates;
  context.SettingsPromptTemplatePaths.Count = 2;

  PromptContextOptions options = {};
  options.WorkingDirectory = SVL("/work");
  options.NoSkills = true;

  RuntimeSetPromptOptions(&rt, &options);
  CHECK_ALL(StrView_EqCStr(rt.PromptOptions.WorkingDirectory, "/work"),
            rt.PromptOptions.NoSkills,
            rt.PromptOptions.SettingsSkillPathCount == 0,
            rt.PromptOptions.SettingsPromptTemplatePathCount == 0);

  rt.Prompt = &context;
  RuntimeSetPromptOptions(&rt, &options);
  CHECK_ALL(rt.PromptOptions.SettingsSkillPaths == settingsSkills,
            rt.PromptOptions.SettingsSkillPathCount == 1,
            rt.PromptOptions.SettingsPromptTemplatePaths == settingsTemplates,
            rt.PromptOptions.SettingsPromptTemplatePathCount == 2);

  InteractiveRuntime_Release(&rt);
  ArenaRelease(&arena);
}

void TestRuntimePersistSettings() {
  Arena arena = ArenaCreate();
  const char* dir = "clanker_runtime_settings_test";
  const char* path = "clanker_runtime_settings_test/settings.json";
  remove(path);

  InteractiveRuntime rt = {};
  InteractiveRuntime_Init(&rt, &arena);
  CHECK(rt.Rounded);

  rt.Spec = ProviderRegistry_At(0);
  CHECK(rt.Spec != nullptr);
  AgentHarness harness = {};
  harness.Config.Compaction = AgentCompactionPolicy_Default();
  rt.Harness = &harness;
  rt.CurrentModel.Id = rt.Spec->DefaultModel;
  rt.SettingsPath = SV(path);
  rt.PermissionMode = AgentPermissionMode::Yolo;
  rt.PermissionReviewerModel = SVL("reviewer/model");
  rt.YoloAcknowledged = true;
  rt.Rounded = false;
  CHECK(RuntimePersistSettings(&rt));

  AppSettings loaded = {};
  CHECK(AppSettings_Load(&arena, SV(path), &loaded) ==
        AppSettingsLoadResult::Success);
  CHECK_ALL(!loaded.Rounded, loaded.PermissionMode == AgentPermissionMode::Yolo,
            StrView_EqCStr(loaded.PermissionReviewerModel, "reviewer/model"),
            loaded.YoloAcknowledged);

  rt.PermissionMode = AgentPermissionMode::Ask;
  rt.PersistedPermissionMode = AgentPermissionMode::Yolo;
  rt.PermissionModeOverridden = true;
  rt.Rounded = true;
  CHECK(RuntimePersistSettings(&rt));
  CHECK(AppSettings_Load(&arena, SV(path), &loaded) ==
        AppSettingsLoadResult::Success);
  CHECK_ALL(loaded.PermissionMode == AgentPermissionMode::Yolo,
            loaded.Rounded);

  rt.PermissionMode = AgentPermissionMode::Yolo;
  rt.PersistedPermissionMode = AgentPermissionMode::Ask;
  rt.PermissionModeOverridden = true;
  rt.Rounded = false;
  CHECK(RuntimePersistSettings(&rt));
  CHECK(AppSettings_Load(&arena, SV(path), &loaded) ==
        AppSettingsLoadResult::Success);
  CHECK_ALL(loaded.PermissionMode == AgentPermissionMode::Ask,
            !loaded.Rounded);

  InteractiveRuntime_Release(&rt);
  remove(path);
#if defined(_WIN32)
  _rmdir(dir);
#else
  rmdir(dir);
#endif
  ArenaRelease(&arena);
}

void TestRuntimePermissionCommandEdges() {
  Arena arena = ArenaCreate();
  const char* dir = "clanker_runtime_permission_test";
  const char* path = "clanker_runtime_permission_test/settings.json";
  remove(path);
#if defined(_WIN32)
  _rmdir(dir);
#else
  rmdir(dir);
#endif

  MemoryRepository memory = {};
  MemoryRepository_Init(&memory);
  SessionRepository sessions = MemoryRepository_AsRepository(&memory);
  SessionCreateOptions create = {};
  create.ProjectKey = SVL("permission-command-test");
  create.WorkingDirectory = SVL("C:\\work");
  SessionLog log = {};
  SessionInfo info = {};
  CHECK(SessionResult_IsOk(
      sessions.Create(sessions.Ctx, &arena, &create, &log, &info)));

  const ProviderSpec* spec = ProviderRegistry_At(0);
  CHECK(spec != nullptr);
  Model model = spec->CreateModel(&arena, spec, spec->DefaultModel);
  AgentHarnessConfig config = {};
  config.ModelInfo = model;
  config.ProviderImpl = spec->CreateProvider(&arena, spec);
  config.PermissionMode = AgentPermissionMode::Auto;
  config.PermissionReviewerModel =
      spec->CreateModel(&arena, spec, AGENT_PERMISSION_DEFAULT_REVIEWER_MODEL);
  config.WorkingDirectory = create.WorkingDirectory;

  AgentHarness harness = {};
  CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));

  InteractiveRuntime rt = {};
  InteractiveRuntime_Init(&rt, &arena);
  rt.SettingsPath = SV(path);
  rt.Spec = spec;
  rt.CurrentModel = model;
  rt.Harness = &harness;
  rt.Sessions = &sessions;
  rt.PermissionMode = AgentPermissionMode::Auto;
  rt.PersistedPermissionMode = AgentPermissionMode::Auto;
  rt.PermissionReviewerModel = AGENT_PERMISSION_DEFAULT_REVIEWER_MODEL;

  u32 catalogCount = 0;
  TuiCommandSpec* catalog =
      RuntimeCommandCatalog(&rt, &arena, &catalogCount);
  CHECK(catalogCount == AppCommandSpec_Count() - 1);
  u32 catalogIndex = 0;
  for (u32 i = 0; i < AppCommandSpec_Count(); i++) {
    const AppCommandSpec* command = AppCommandSpec_At(i);
    if (command->Kind == AppCommandKind::Skill) continue;
    CHECK_ALL(StrView_Eq(catalog[catalogIndex].Name, command->Name),
              StrView_Eq(catalog[catalogIndex].Alias, command->Alias),
              StrView_Eq(catalog[catalogIndex].Description,
                         command->Description));
    if (command->Kind == AppCommandKind::Permissions)
      CHECK(catalogIndex == 12);
    catalogIndex++;
  }

  RuntimeCommandCapture capture = {};
  AppCommandUx ux = {&capture, CaptureRuntimeNotice, CaptureRuntimeError};
  AppCommandHandlers handlers = RuntimeCommandHandlers(&rt);

  AppCommandDispatchResult report =
      RuntimeDispatchCommand(&rt, SVL("/permissions"), &handlers, &ux);
  CHECK_ALL(report.Handled && report.Action == AppLoopAction::Continue,
            capture.Notices == 1,
            StrView_Contains(capture.LastNotice, "Permission mode: auto"),
            StrView_Contains(capture.LastNotice,
                             "Permission reviewer model: z-ai/glm-5.2"),
            StrView_Contains(capture.LastNotice, "Session grants: 0"));

  rt.PermissionMode = AgentPermissionMode::Ask;
  rt.PersistedPermissionMode = AgentPermissionMode::Yolo;
  rt.PermissionModeOverridden = true;
  AgentPermissionMode beforeMode =
      AgentHarness_GetPermissionMode(&harness);
  u32 errorsBefore = capture.Errors;
  AppCommandDispatchResult invalid =
      RuntimeDispatchCommand(&rt, SVL("/permissions nope"), &handlers, &ux);
  CHECK_ALL(invalid.Handled && invalid.Action == AppLoopAction::Continue,
            capture.Errors == errorsBefore + 1,
            StrView_Eq(capture.LastError,
                       AppCommand_Usage(AppCommandKind::Permissions)),
            AgentHarness_GetPermissionMode(&harness) == beforeMode,
            rt.PermissionMode == AgentPermissionMode::Ask,
            rt.PersistedPermissionMode == AgentPermissionMode::Yolo,
            rt.PermissionModeOverridden);

  RuntimeDispatchCommand(&rt, SVL("/permissions ask"), &handlers, &ux);
  CHECK_ALL(AgentHarness_GetPermissionMode(&harness) ==
                AgentPermissionMode::Ask,
            rt.PermissionMode == AgentPermissionMode::Ask,
            rt.PersistedPermissionMode == AgentPermissionMode::Ask,
            !rt.PermissionModeOverridden);
  rt.Rounded = false;
  CHECK(RuntimePersistSettings(&rt));
  AppSettings loaded = {};
  CHECK(AppSettings_Load(&arena, SV(path), &loaded) ==
        AppSettingsLoadResult::Success);
  CHECK_ALL(loaded.PermissionMode == AgentPermissionMode::Ask,
            !loaded.Rounded);

  RuntimeDispatchCommand(&rt, SVL("/permissions auto"), &handlers, &ux);
  CHECK_ALL(AgentHarness_GetPermissionMode(&harness) ==
                AgentPermissionMode::Auto,
            rt.PermissionMode == AgentPermissionMode::Auto,
            rt.PersistedPermissionMode == AgentPermissionMode::Auto,
            !rt.PermissionModeOverridden);

  CHECK(!rt.YoloAcknowledged);
  RuntimeDispatchCommand(&rt, SVL("/permissions yolo"), &handlers, &ux);
  CHECK_ALL(AgentHarness_GetPermissionMode(&harness) ==
                AgentPermissionMode::Yolo,
            rt.PermissionMode == AgentPermissionMode::Yolo,
            !rt.YoloAcknowledged);

  rt.YoloAcknowledged = true;
  RuntimeDispatchCommand(&rt, SVL("/permissions reset"), &handlers, &ux);
  CHECK_ALL(AgentHarness_GetPermissionMode(&harness) ==
                AgentPermissionMode::Ask,
            rt.PermissionMode == AgentPermissionMode::Ask,
            rt.PersistedPermissionMode == AgentPermissionMode::Ask,
            !rt.PermissionModeOverridden, rt.YoloAcknowledged);

  InteractiveRuntime_Release(&rt);
  CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
  sessions.Close(sessions.Ctx);
  remove(path);
#if defined(_WIN32)
  _rmdir(dir);
#else
  rmdir(dir);
#endif
  ArenaRelease(&arena);
}

void TestRuntimeRefreshPermissionReviewer() {
  Arena arena = ArenaCreate();
  ProviderSpec first = {
      SVL("provider-one"), SVL("Provider One"), SVL("model-one"),
      nullptr,           nullptr,           nullptr,
      TestReviewerModel, nullptr};
  ProviderSpec second = {
      SVL("provider-two"), SVL("Provider Two"), SVL("model-two"),
      nullptr,            nullptr,            nullptr,
      TestReviewerModel, nullptr};

  InteractiveRuntime rt = {};
  InteractiveRuntime_Init(&rt, &arena);
  AgentHarness harness = {};
  rt.Harness = &harness;
  rt.Spec = &first;
  rt.PermissionReviewerModel = SVL("reviewer-one");
  CHECK(RuntimeRefreshPermissionReviewer(&rt));
  CHECK_ALL(StrView_EqCStr(harness.Config.PermissionReviewerModel.Id,
                           "reviewer-one"),
            StrView_Eq(harness.Config.PermissionReviewerModel.Provider,
                       first.Id));

  rt.Spec = &second;
  rt.PermissionReviewerModel = SVL("reviewer-two");
  CHECK(RuntimeRefreshPermissionReviewer(&rt));
  CHECK_ALL(StrView_EqCStr(harness.Config.PermissionReviewerModel.Id,
                           "reviewer-two"),
            StrView_Eq(harness.Config.PermissionReviewerModel.Provider,
                       second.Id),
            !StrView_Eq(harness.Config.PermissionReviewerModel.Provider,
                        first.Id));

  InteractiveRuntime_Release(&rt);
  ArenaRelease(&arena);
}

void TestRuntimeSelectionPrecedence() {
  Arena arena = ArenaCreate();
  const char* dir = "clanker_runtime_selection_test";
  const char* path = "clanker_runtime_selection_test/settings.json";
  remove(path);
#if defined(_WIN32)
  _rmdir(dir);
#else
  rmdir(dir);
#endif

  RuntimeSelection defaults = LoadRuntimeSelection(&arena, SV(path), nullptr);
  CHECK_ALL(defaults.Spec == ProviderRegistry_At(0),
            StrView_Eq(defaults.ModelId, defaults.Spec->DefaultModel),
            defaults.PermissionMode == AgentPermissionMode::Auto,
            StrView_Eq(defaults.PermissionReviewerModel,
                       AGENT_PERMISSION_DEFAULT_REVIEWER_MODEL),
            !defaults.YoloAcknowledged);

  CHECK(ConfigFile_WriteAtomic(
      &arena, SV(path),
      SVL("{\"provider\":\"openrouter\",\"model\":\"configured\","
          "\"permissionMode\":\"yolo\","
          "\"permissionReviewerModel\":\"reviewer/model\","
          "\"yoloAcknowledged\":true}")));
  RuntimeSelection configured = LoadRuntimeSelection(&arena, SV(path), "override");
  CHECK_ALL(StrView_EqCStr(configured.ModelId, "override"),
            configured.PermissionMode == AgentPermissionMode::Yolo,
            StrView_EqCStr(configured.PermissionReviewerModel,
                           "reviewer/model"),
            configured.YoloAcknowledged);

  CHECK(ConfigFile_WriteAtomic(&arena, SV(path), SVL("{bad")));
  RuntimeSelection malformed = LoadRuntimeSelection(&arena, SV(path), nullptr);
  CHECK_ALL(malformed.Spec == ProviderRegistry_At(0),
            StrView_Eq(malformed.ModelId, malformed.Spec->DefaultModel),
            malformed.PermissionMode == AgentPermissionMode::Auto,
            StrView_Eq(malformed.PermissionReviewerModel,
                       AGENT_PERMISSION_DEFAULT_REVIEWER_MODEL));

  remove(path);
#if defined(_WIN32)
  _rmdir(dir);
#else
  rmdir(dir);
#endif
  ArenaRelease(&arena);
}

// The seam must never block a run that has no user behind it. `-p` and
// `--mode json` both set Interactive = false, and this is the only thing
// standing between that and a headless run parked on a modal forever.
void TestRuntimeAskUserNonInteractive() {
  Arena arena = ArenaCreate();
  InteractiveRuntime rt = {};
  InteractiveRuntime_Init(&rt, &arena);
  rt.Interactive = false;

  AgentAskUser ask = RuntimeAskUser(&rt);
  CHECK_ALL(ask.User == &rt, ask.Ask != nullptr);

  StrView choices[] = {SVL("Continue"), SVL("Stop")};
  AgentAskRequest request = {};
  request.Question = SVL("Keep going?");
  request.Choices = choices;
  request.ChoiceCount = 2;
  AgentAskResponse response = AgentAskUser_Invoke(&ask, &arena, &request);
  CHECK(response.Status == AgentAskStatus::Unavailable);

  AgentAskRequest freeText = {};
  freeText.Question = SVL("Which database?");
  response = AgentAskUser_Invoke(&ask, &arena, &freeText);
  CHECK(response.Status == AgentAskStatus::Unavailable);

  // An empty question is refused even with a user present, since there is
  // nothing to put on screen.
  rt.Interactive = true;
  AgentAskRequest empty = {};
  response = AgentAskUser_Invoke(&ask, &arena, &empty);
  CHECK(response.Status == AgentAskStatus::Unavailable);

  InteractiveRuntime_Release(&rt);
  ArenaRelease(&arena);
}

void TestEventPumpTranscriptLifecycle() {
  Arena arena = ArenaCreate();
  MemoryRepository memory = {};
  MemoryRepository_Init(&memory);
  SessionRepository sessions = MemoryRepository_AsRepository(&memory);
  SessionCreateOptions create = {};
  create.ProjectKey = SVL("event-pump-test");
  create.WorkingDirectory = SVL("C:\\work");
  SessionLog log = {};
  SessionInfo info = {};
  CHECK(SessionResult_IsOk(
      sessions.Create(sessions.Ctx, &arena, &create, &log, &info)));
  Array<SessionRecord> root = {};
  CHECK(SessionResult_IsOk(SessionLog_ReadAll(&log, &arena, &root)));
  SessionRecord seed[2] = {};
  seed[0].Kind = SessionRecordKind::Message;
  seed[0].EntryId = DurableId_Generate();
  seed[0].MessageEntry.ParentId = root.Data[0].EntryId;
  seed[0].MessageEntry.Value = Message_User(&arena, SVL("saved prompt"));
  seed[1].Kind = SessionRecordKind::Message;
  seed[1].EntryId = DurableId_Generate();
  seed[1].MessageEntry.ParentId = seed[0].EntryId;
  seed[1].MessageEntry.Value = Message_AssistantEmpty(&arena);
  Array_Push(&seed[1].MessageEntry.Value.Content,
             Content_Text(SVL("saved reply")));
  CHECK(SessionResult_IsOk(SessionLog_AppendBatch(&log, seed, 2)));

  AgentHarnessConfig config = {};
  config.ModelInfo.Id = SVL("test-model");
  config.ModelInfo.Provider = SVL("test");
  AgentHarness harness = {};
  CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
  Tui tui = {};
  TuiOpts opts = {};
  opts.Color = false;
  Tui_InitModel(&tui, &arena, opts);
  LineEdit_InsertText(&tui.Edit, SVL("draft"));
  usize baseline = arena.Used;
  HarnessEventPump pump = {};
  HarnessEventSink sink = {&tui, TuiBridge_HarnessEvent};
  CHECK(SessionResult_IsOk(
      StartHarnessEventPump(&harness, sink, &tui, &pump)));
  // Hydration has completed when Start returns, and its arena is already gone.
  CHECK_ALL(pump.Started, arena.Used == baseline,
            tui.Model.Entries.Count == 2);
  if (tui.Model.Entries.Count == 2) {
    CHECK(StrView_EqCStr(
        StrBuf_View(&tui.Model.Entries.Data[0].Text.Content), "saved prompt"));
    CHECK(StrView_EqCStr(
        StrBuf_View(&tui.Model.Entries.Data[1].Text.Content), "saved reply"));
  }

  if (pump.Started) {
    // Inject a lag marker deterministically instead of racing a producer
    // against the queue capacity. Recovery must replace, not append history.
    Tui_CommandNotice(&tui, SVL("stale display"));
    Mutex_Lock(&pump.WatchGuard);
    Mutex_Lock(&pump.Watch.Lock);
    DurableId previousWatch = pump.Watch.Id;
    pump.Watch.Lagged = true;
    Cond_Signal(&pump.Watch.Changed);
    Mutex_Unlock(&pump.Watch.Lock);
    Mutex_Unlock(&pump.WatchGuard);
    bool recovered = false;
    for (u32 i = 0; i < 1000 && !recovered; i++) {
      Mutex_Lock(&pump.WatchGuard);
      recovered = !DurableId_Eq(previousWatch, pump.Watch.Id);
      Mutex_Unlock(&pump.WatchGuard);
      if (!recovered) Sleep_Ms(5);
    }
    CHECK(recovered);
    StopHarnessEventPump(&pump);
    CHECK_ALL(arena.Used == baseline, tui.Model.Entries.Count == 2,
              pump.CompletionMem.Base == nullptr);
  }

  InteractiveRuntime rt = {};
  InteractiveRuntime_Init(&rt, &arena);
  rt.Harness = &harness;
  rt.EventSink = sink;
  rt.EventPump = &pump;
  rt.TuiView = &tui;
  for (u32 i = 0; i < 3; i++) {
    Tui_CommandNotice(&tui, SVL("old session"));
    CHECK(RuntimeRestartWatch(&rt, nullptr));
    CHECK_ALL(arena.Used == baseline, tui.Model.Entries.Count == 2,
              StrView_EqCStr(LineEdit_Text(&tui.Edit), "draft"));
    StopHarnessEventPump(&pump);
  }
  InteractiveRuntime_Release(&rt);

  HarnessEventSink quiet = {};
  CHECK(SessionResult_IsOk(
      StartHarnessEventPump(&harness, quiet, nullptr, &pump)));
  CHECK_ALL(arena.Used == baseline, pump.Watch.Mem.Used == 0);
  StopHarnessEventPump(&pump);
  CHECK(harness.WatcherCount == 0);
  CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
  CHECK(!SessionResult_IsOk(
      StartHarnessEventPump(&harness, quiet, &tui, &pump)));
  CHECK_ALL(!pump.Started, !pump.Watch.Initialized,
            !pump.WatchGuardInitialized, pump.CompletionMem.Base == nullptr,
            arena.Used == baseline);
  Tui_Stop(&tui);
  sessions.Close(sessions.Ctx);
  ArenaRelease(&arena);
}

struct JsonCapture {
  Arena* Mem;
  Array<StrView> Lines;
  StrBuf Text;
  u32 Writes;
  u32 FailAt;
  u32 Deltas;
  u32 Records;
  u32 Results;
  u32 ToolStarts;
  u32 ToolUpdates;
  u32 ToolEnds;
  u32 Invalid;
  u32 PauseAtDelta;
  volatile i32 Paused;
  volatile i32 Resume;
};

bool CaptureJsonLine(void* user, StrView line) {
  JsonCapture* capture = (JsonCapture*)user;
  capture->Writes++;
  if (capture->FailAt && capture->Writes == capture->FailAt) return false;
  Array_Push(&capture->Lines, StrView_Dup(capture->Mem, line));
  if (!line.Length || line.Data[line.Length - 1] != '\n') {
    capture->Invalid++;
    return true;
  }
  JsonValue* value = Json_Parse(capture->Mem, line);
  if (!Json_IsObject(value)) {
    capture->Invalid++;
    return true;
  }
  StrView type = Json_GetStr(value, "type");
  if (StrView_EqCStr(type, "text_delta")) {
    capture->Deltas++;
    StrBuf_Append(&capture->Text, Json_GetStr(value, "text"));
    if (capture->PauseAtDelta == capture->Deltas) {
      Atomic_StoreI32(&capture->Paused, 1);
      while (!Atomic_LoadI32(&capture->Resume)) Sleep_Ms(1);
    }
  } else if (StrView_EqCStr(type, "record")) {
    capture->Records++;
  } else if (StrView_EqCStr(type, "result")) {
    capture->Results++;
  } else if (StrView_EqCStr(type, "tool_start")) {
    capture->ToolStarts++;
  } else if (StrView_EqCStr(type, "tool_update")) {
    capture->ToolUpdates++;
  } else if (StrView_EqCStr(type, "tool_end")) {
    capture->ToolEnds++;
  } else if (StrView_EqCStr(type, "events_dropped") || !type.Length) {
    capture->Invalid++;
  }
  return true;
}

struct JsonBurst {
  AgentHarness* Harness;
  u32 Count;
  volatile i32 Emitted;
  SessionResult Result;
};

bool JsonBurstStream(void* user, Arena* arena, const Model*, const LlmContext*,
                     const StreamOptions* options, StreamSink sink, Message* out) {
  JsonBurst* burst = (JsonBurst*)user;
  *out = Message_AssistantEmpty(arena);
  out->Stop = StopReason::Stop;
  StrBuf text = StrBuf_Make(arena, 32);
  for (u32 i = 0; i < burst->Count; i++) {
    if (options->Abort && Atomic_LoadI32(options->Abort)) {
      out->Stop = StopReason::Aborted;
      out->ErrorMessage = SVL("cancelled");
      return false;
    }
    StreamEvent event = {};
    event.Kind = StreamEventKind::TextDelta;
    event.Delta = SVL("x\n\"");
    StreamSink_Emit(sink, event);
    StrBuf_Append(&text, event.Delta);
    Atomic_StoreI32(&burst->Emitted, (i32)(i + 1));
  }
  Array_Push(&out->Content, Content_Text(StrBuf_View(&text)));
  return true;
}

void RunJsonBurst(void* user) {
  JsonBurst* burst = (JsonBurst*)user;
  burst->Result = AgentHarness_Run(burst->Harness, SVL("burst"));
}

struct JsonParallelTools {
  volatile i32 Ready[2];
};

bool JsonProgressTool(void* user, Arena* arena, StrView id, const ToolArgs*,
                      volatile i32* abort, const ToolUpdateSink* updates,
                      ToolResult* out, StrBuf* error) {
  JsonParallelTools* tools = (JsonParallelTools*)user;
  u32 index = StrView_EqCStr(id, "parallel-a") ? 0 : 1;
  Atomic_StoreI32(&tools->Ready[index], 1);
  for (u32 i = 0; i < 1000 && !Atomic_LoadI32(&tools->Ready[1 - index]); i++)
    Sleep_Ms(5);
  if (!Atomic_LoadI32(&tools->Ready[1 - index])) {
    StrBuf_AppendCStr(error, "parallel tool did not start");
    return false;
  }
  out->Content = Array_Make<Content>(arena, 1);
  Array_Push(&out->Content, Content_Text(id));
  for (u32 i = 0; i < 64; i++) {
    if (abort && Atomic_LoadI32(abort)) {
      StrBuf_AppendCStr(error, "cancelled");
      return false;
    }
    ToolUpdateSink_Emit(updates, out);
    if (i % 8 == 0) Sleep_Ms(1);
  }
  return true;
}

void TestJsonOutput() {
  Arena arena = ArenaCreate();
  MemoryRepository memory = {};
  MemoryRepository_Init(&memory);
  SessionRepository repository = MemoryRepository_AsRepository(&memory);
  SessionCreateOptions options = {};
  options.ProjectKey = SVL("json-output");
  options.WorkingDirectory = SVL("C:\\work\\clanker");
  SessionLog log = {};
  SessionInfo info = {};
  CHECK(SessionResult_IsOk(
      repository.Create(repository.Ctx, &arena, &options, &log, &info)));
  JsonBurst burst = {};
  burst.Count = 2048;
  AgentHarnessConfig config = {};
  config.ModelInfo.Id = SVL("burst");
  config.ModelInfo.Provider = SVL("test");
  config.ProviderImpl = {&burst, JsonBurstStream};
  JsonParallelTools parallel = {};
  Tool tool = {};
  tool.Name = SVL("progress");
  tool.Description = SVL("parallel progress");
  tool.ParamsJsonSchema = SVL("{\"type\":\"object\"}");
  tool.Ctx = &parallel;
  tool.Execute = JsonProgressTool;
  config.Tools = &tool;
  config.ToolCount = 1;
  config.ToolExec = ToolExecution::Parallel;
  config.MaxParallelTools = 2;
  AgentHarness harness = {};
  CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
  burst.Harness = &harness;
  AgentHarnessWatch* watch = ArenaPushStruct(&arena, AgentHarnessWatch);
  *watch = {};
  CHECK(SessionResult_IsOk(AgentHarness_Watch(&harness, 2, watch)));

  JsonCapture capture = {};
  capture.Mem = &arena;
  capture.Lines = Array_Make<StrView>(&arena, 32);
  capture.Text = StrBuf_Make(&arena, 32);
  capture.PauseAtDelta = 4;
  HarnessJsonOutput output = {};
  CHECK(SessionResult_IsOk(StartHarnessJsonOutput(
      &harness, &output, {&capture, CaptureJsonLine})));
  CHECK(harness.WatcherCount == 1);
  Thread worker = {};
  bool started = Thread_Start(&worker, RunJsonBurst, &burst);
  CHECK(started);
  if (started) {
    for (u32 i = 0; i < 1000 && !Atomic_LoadI32(&capture.Paused); i++)
      Sleep_Ms(5);
    CHECK(Atomic_LoadI32(&capture.Paused) == 1);
    CHECK(Atomic_LoadI32(&burst.Emitted) == 3);
    // A stalled writer stops the producer, not just its queue consumer.
    Sleep_Ms(10);
    CHECK(Atomic_LoadI32(&burst.Emitted) == 3);
    Atomic_StoreI32(&capture.Resume, 1);
    Thread_Join(&worker);
    CHECK(SessionResult_IsOk(burst.Result));
  }
  CHECK_ALL(capture.Deltas == burst.Count, capture.Records == 2,
            capture.Invalid == 0, output.Scratch.Committed <= 64 * 1024,
            watch->Lagged || watch->DroppedTransient > 0);
  AgentTranscriptSnapshot snapshot = {};
  CHECK(SessionResult_IsOk(
      AgentHarness_SnapshotTranscript(&arena, &harness, 1, &snapshot)));
  CHECK(snapshot.Transcript.Count == 1);
  if (snapshot.Transcript.Count)
    CHECK(StrView_Eq(snapshot.Transcript.Data[0].Content.Data[0].Text,
                     StrBuf_View(&capture.Text)));

  SessionResult notice = SessionResult_Ok();
  notice.Message = SVL("stopped at the \"turn limit\"\n");
  CHECK(SessionResult_IsOk(FinishHarnessJsonOutput(&output, notice)));
  CHECK_ALL(capture.Results == 1, capture.Invalid == 0, !output.Started,
            output.Scratch.Base == nullptr, !harness.Config.Events.OnEvent,
            capture.Lines.Count == burst.Count + 7);
  if (capture.Lines.Count >= 2) {
    CHECK(StrView_EqCStr(capture.Lines.Data[capture.Lines.Count - 1],
        "{\"type\":\"result\",\"ok\":true,\"message\":\"stopped at the "
        "\\\"turn limit\\\"\\n\"}\n"));
    CHECK(StrView_Contains(capture.Lines.Data[capture.Lines.Count - 2],
                           "\"type\":\"agent_end\""));
  }
  CHECK(SessionResult_IsOk(AgentHarness_WatchClose(watch)));

  // An output failure cancels the producer, is sticky, and cannot be hidden by
  // a successful run or by attempting to append a success-shaped terminator.
  capture.FailAt = capture.Writes + 4;
  Atomic_StoreI32(&burst.Emitted, 0);
  CHECK(SessionResult_IsOk(StartHarnessJsonOutput(
      &harness, &output, {&capture, CaptureJsonLine})));
  SessionResult run = AgentHarness_Run(&harness, SVL("write failure"));
  CHECK(Atomic_LoadI32(&burst.Emitted) < (i32)burst.Count);
  CHECK_ALL(output.Failure.Status == SessionStatus::IoError,
            Atomic_LoadI32(&harness.AbortFlag) != 0);
  CHECK(FinishHarnessJsonOutput(&output, run).Status == SessionStatus::IoError);
  CHECK_ALL(capture.Writes == capture.FailAt, capture.Results == 1,
            output.Scratch.Base == nullptr);

  // Final-line failures also override a successful run's exit verdict.
  capture.FailAt = capture.Writes + 1;
  CHECK(SessionResult_IsOk(StartHarnessJsonOutput(
      &harness, &output, {&capture, CaptureJsonLine})));
  CHECK(FinishHarnessJsonOutput(&output, SessionResult_Ok()).Status ==
        SessionStatus::IoError);
  CHECK(capture.Writes == capture.FailAt);

  capture.FailAt = 0;
  CHECK(SessionResult_IsOk(StartHarnessJsonOutput(
      &harness, &output, {&capture, CaptureJsonLine})));
  AgentHarnessEvent invalid = {};
  invalid.Kind = AgentHarnessEventKind::DurableRecord;
  harness.Config.Events.OnEvent(harness.Config.Events.User, &invalid);
  CHECK(!SessionResult_IsOk(output.Failure));
  u32 writes = capture.Writes;
  CHECK(!SessionResult_IsOk(FinishHarnessJsonOutput(&output, SessionResult_Ok())));
  CHECK(capture.Writes == writes);

  CHECK(SessionResult_IsOk(StartHarnessJsonOutput(
      &harness, &output, {&capture, CaptureJsonLine})));
  SessionResult failed = SessionResult_Error(SessionStatus::Cancelled, SVL("cancelled"));
  CHECK(FinishHarnessJsonOutput(&output, failed).Status == SessionStatus::Cancelled);
  CHECK(StrView_EqCStr(capture.Lines.Data[capture.Lines.Count - 1],
                      "{\"type\":\"result\",\"ok\":false,\"error\":\"cancelled\"}\n"));

  ScriptToolCall calls[2] = {
      {SVL("progress"), SVL("parallel-a"), SVL("{}")},
      {SVL("progress"), SVL("parallel-b"), SVL("{}")}};
  ScriptStep steps[2] = {};
  steps[0].Tools = calls;
  steps[0].ToolCount = 2;
  steps[1].Text = SVL("done");
  steps[1].Stop = StopReason::Stop;
  ScriptProvider script = {steps, 2, 0};
  harness.Config.ProviderImpl = ScriptProvider_Make(&script);
  u32 previousRecords = capture.Records;
  CHECK(SessionResult_IsOk(StartHarnessJsonOutput(
      &harness, &output, {&capture, CaptureJsonLine})));
  run = AgentHarness_Run(&harness, SVL("parallel tools"));
  CHECK(SessionResult_IsOk(FinishHarnessJsonOutput(&output, run)));
  CHECK_ALL(capture.ToolStarts == 2, capture.ToolUpdates == 128,
            capture.ToolEnds == 2, capture.Records == previousRecords + 5,
            capture.Invalid == 0);
  CHECK(StrView_EqCStr(capture.Lines.Data[capture.Lines.Count - 1],
                      "{\"type\":\"result\",\"ok\":true}\n"));

  // Exercise the actual stdio writer without replacing this test's stdout.
#if defined(_WIN32)
  FILE* readOnly = fopen("NUL", "rb");
#else
  FILE* readOnly = fopen("/dev/null", "rb");
#endif
  CHECK(readOnly != nullptr);
  if (readOnly) {
    CHECK(SessionResult_IsOk(StartHarnessJsonOutput(&harness, &output)));
    output.Writer.User = readOnly;
    CHECK(FinishHarnessJsonOutput(&output, SessionResult_Ok()).Status ==
          SessionStatus::IoError);
    fclose(readOnly);
  }
#if !defined(_WIN32)
  int pipeFds[2] = {};
  bool piped = pipe(pipeFds) == 0;
  CHECK(piped);
  if (piped) {
    close(pipeFds[0]);
    FILE* brokenPipe = fdopen(pipeFds[1], "w");
    CHECK(brokenPipe != nullptr);
    if (brokenPipe) {
      CHECK(SessionResult_IsOk(StartHarnessJsonOutput(&harness, &output)));
      output.Writer.User = brokenPipe;
      // Buffered fwrite succeeds; flush fails with EPIPE, not SIGPIPE death.
      CHECK(FinishHarnessJsonOutput(&output, SessionResult_Ok()).Status ==
            SessionStatus::IoError);
      fclose(brokenPipe);
    } else {
      close(pipeFds[1]);
    }
  }
#endif
  CHECK(SessionResult_IsOk(AgentHarness_Close(&harness)));
  repository.Close(repository.Ctx);
  ArenaRelease(&arena);
}

void TestApp() {
  TestSessionProjectKey();
  TestProjectKeyComparison();
  TestRuntimeCommandArena();
  TestRuntimeSetPromptOptions();
  TestRuntimePersistSettings();
  TestRuntimePermissionCommandEdges();
  TestRuntimeRefreshPermissionReviewer();
  TestRuntimeSelectionPrecedence();
  TestRuntimeAskUserNonInteractive();
  TestEventPumpTranscriptLifecycle();
  TestJsonOutput();
  TestConfigPaths();
  TestTrustChoiceMenu();
  TestCommandResultMapping();
}
