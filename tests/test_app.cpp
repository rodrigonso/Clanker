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
#include "app/provider_registry.h"
#include "app/runtime.h"
#include "app/session_switch.h"
#include "app/settings.h"
#include "base/arena.h"
#include "base/config_file.h"
#include "check.h"
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

void TestTrimAscii() {
  CHECK(StrView_EqCStr(TrimAscii(SVL("  hello \t\r\n")), "hello"));
  CHECK(StrView_EqCStr(TrimAscii(SVL("hello")), "hello"));
  CHECK(TrimAscii(SVL("   ")).Length == 0);
  CHECK(TrimAscii(StrView{}).Length == 0);
  // Interior whitespace is preserved; only the edges are trimmed.
  CHECK(StrView_EqCStr(TrimAscii(SVL("  a b  ")), "a b"));
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
  TestConfigPaths();
  TestTrustChoiceMenu();
  TestTrimAscii();
  TestCommandResultMapping();
}
