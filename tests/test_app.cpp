// Application-layer logic that used to be unreachable from tests.
//
// Everything here lived inside main.cpp, which has no library target, so none
// of it could be exercised. The behaviour is unchanged; only its address is.
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include "app/config_paths.h"
#include "app/auth_flow.h"
#include "app/commands.h"
#include "app/event_pump.h"
#include "app/permission_reviewer.h"
#include "app/plain_select.h"
#include "app/provider_registry.h"
#include "app/runtime.h"
#include "app/session_switch.h"
#include "app/settings.h"
#include "app/tui_bridge.h"
#include "auth/store.h"
#include "base/arena.h"
#include "base/config_file.h"
#include "base/sysinfo.h"
#include "check.h"
#include "json/json.h"
#include "script_provider.h"
#include "session/metadata.h"
#include "session/memory_repository.h"
#include "tools/fs.h"

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

Model TestChatModel(Arena* arena, const ProviderSpec* spec, StrView modelId) {
  Model model = {};
  model.Id = StrView_Dup(arena, modelId);
  model.Name = StrView_Dup(arena, modelId);
  model.Provider = spec ? StrView_Dup(arena, spec->Id) : StrView{};
  return model;
}

bool UnexpectedChatStream(void*, Arena*, const Model*, const LlmContext*,
                           const StreamOptions*, StreamSink, Message*) {
  CHECK(false);
  return false;
}

Provider TestChatProvider(Arena*, const ProviderSpec*) {
  Provider provider = {};
  provider.Stream = UnexpectedChatStream;
  return provider;
}

bool NoRuntimeAuth(void*, Arena*, StrView, volatile i32*, ProviderAuth*) {
  return false;
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
  rt.PermissionReviewerModel = SVL("jev-1.13.0");
  rt.PermissionReviewerThreshold = 0.85;
  rt.YoloAcknowledged = true;
  rt.Rounded = false;
  CHECK(RuntimePersistSettings(&rt));

  AppSettings loaded = {};
  CHECK(AppSettings_Load(&arena, SV(path), &loaded) ==
        AppSettingsLoadResult::Success);
  CHECK_ALL(!loaded.Rounded, loaded.PermissionMode == AgentPermissionMode::Yolo,
            loaded.PermissionReviewerThreshold == 0.85,
            StrView_EqCStr(loaded.PermissionReviewerModel, "jev-1.13.0"),
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
  config.PermissionReviewerModel = AGENT_PERMISSION_DEFAULT_REVIEWER_MODEL;
  config.PermissionReviewer =
      TypeSafePermissionReviewer_Make(&arena, config.PermissionReviewerModel);
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
    CHECK(catalog[catalogIndex].PrivateArguments ==
          (command->Kind == AppCommandKind::Auth ||
           command->Kind == AppCommandKind::Permissions));
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
                             "Permission reviewer model: jev-latest"),
            StrView_Contains(capture.LastNotice, "Session grants: 0"));

  {
    i32 revision = Atomic_LoadI32(&harness.Permissions.Revision);
    RuntimeDispatchCommand(&rt, SVL("/permissions threshold"), &handlers, &ux);
    CHECK_ALL(StrView_Contains(capture.LastNotice, "Permission reviewer threshold: 0.95"),
              Atomic_LoadI32(&harness.Permissions.Revision) == revision,
              !Fs_Exists(SV(path)));
    RuntimeDispatchCommand(&rt, SVL("/permissions model"), &handlers, &ux);
    CHECK_ALL(StrView_Contains(capture.LastNotice, "Permission reviewer model: jev-latest"),
              Atomic_LoadI32(&harness.Permissions.Revision) == revision,
              !Fs_Exists(SV(path)));
    AgentPermissionGrant grant = {};
    grant.Key = StrView_Dup(&harness.Permissions.GrantMem, SVL("test-grant"));
    Array_Push(&harness.Permissions.Grants, grant);
    struct ThresholdCase {
      StrView Command;
      f64 Value;
    };
    const ThresholdCase valid[] = {
        {SVL("/permissions threshold 0.9"), 0.9},
        {SVL("/permissions threshold 0"), 0},
        {SVL("/permissions threshold 1"), 1},
        {SVL("/permissions threshold   .85  "), 0.85},
        {SVL("/permissions threshold 9e-1"), 0.9}};
    AppSettings saved = {};
    for (const ThresholdCase& test : valid) {
      u32 errors = capture.Errors;
      revision = Atomic_LoadI32(&harness.Permissions.Revision);
      void* reviewer = harness.Config.PermissionReviewer.User;
      RuntimeDispatchCommand(&rt, test.Command, &handlers, &ux);
      CHECK_ALL(capture.Errors == errors,
                StrView_Contains(capture.LastNotice, "threshold saved and set to"),
                rt.PermissionReviewerThreshold == test.Value,
                harness.Config.PermissionReviewerThreshold == test.Value,
                harness.Config.PermissionReviewer.User != reviewer,
                Atomic_LoadI32(&harness.Permissions.Revision) == revision + 1,
                AgentHarness_PermissionGrantCount(&harness) == 1,
                AgentHarness_GetPermissionMode(&harness) == AgentPermissionMode::Auto);
      CHECK(AppSettings_Load(&arena, SV(path), &saved) == AppSettingsLoadResult::Success);
      CHECK_ALL(saved.PermissionReviewerThreshold == test.Value,
                saved.PermissionMode == AgentPermissionMode::Auto);
    }

    revision = Atomic_LoadI32(&harness.Permissions.Revision);
    void* reviewer = harness.Config.PermissionReviewer.User;
    const StrView invalidThresholds[] = {
        SVL("/permissions threshold -0.1"), SVL("/permissions threshold 1.1"),
        SVL("/permissions threshold nope"), SVL("/permissions threshold nan"),
        SVL("/permissions threshold inf"), SVL("/permissions threshold null"),
        SVL("/permissions threshold 0.9 extra"), SVL("/permissions threshold 0.9%"),
        SVL("/permissions threshold 1e999"), SVL("/permissions threshold 1e-999"),
        SVL("/permissions thresholdx 0.9")};
    for (StrView command : invalidThresholds) {
      u32 errors = capture.Errors;
      RuntimeDispatchCommand(&rt, command, &handlers, &ux);
      CHECK_ALL(capture.Errors == errors + 1,
                StrView_Contains(capture.LastError, "Usage: /permissions"),
                rt.PermissionReviewerThreshold == 0.9,
                harness.Config.PermissionReviewer.User == reviewer,
                Atomic_LoadI32(&harness.Permissions.Revision) == revision);
    }
    CHECK(AppSettings_Load(&arena, SV(path), &saved) == AppSettingsLoadResult::Success);
    CHECK(saved.PermissionReviewerThreshold == 0.9);

    harness.Running = true;
    u32 errors = capture.Errors;
    RuntimeDispatchCommand(&rt, SVL("/permissions threshold"), &handlers, &ux);
    CHECK_ALL(capture.Errors == errors,
              StrView_Contains(capture.LastNotice, "Permission reviewer threshold: 0.9"));
    RuntimeDispatchCommand(&rt, SVL("/permissions threshold 0.8"), &handlers, &ux);
    harness.Running = false;
    CHECK_ALL(capture.Errors == errors + 1,
              StrView_Contains(capture.LastError, "idle"),
              rt.PermissionReviewerThreshold == 0.9,
              harness.Config.PermissionReviewer.User == reviewer,
              Atomic_LoadI32(&harness.Permissions.Revision) == revision);

    rt.SettingsPath = Fs_Join(&arena, SV(path), SVL("not-a-directory.json"));
    errors = capture.Errors;
    RuntimeDispatchCommand(&rt, SVL("/permissions threshold 0.8"), &handlers, &ux);
    rt.SettingsPath = SV(path);
    CHECK_ALL(capture.Errors == errors + 1,
              StrView_Contains(capture.LastError, "was not changed"),
              rt.PermissionReviewerThreshold == 0.9,
              harness.Config.PermissionReviewerThreshold == 0.9,
              harness.Config.PermissionReviewer.User == reviewer,
              Atomic_LoadI32(&harness.Permissions.Revision) == revision);
    CHECK(AppSettings_Load(&arena, SV(path), &saved) == AppSettingsLoadResult::Success);
    CHECK(saved.PermissionReviewerThreshold == 0.9);

    rt.PermissionModeOverridden = true;
    rt.PersistedPermissionMode = AgentPermissionMode::Ask;
    RuntimeDispatchCommand(&rt, SVL("/permissions threshold 0.8"), &handlers, &ux);
    CHECK(AppSettings_Load(&arena, SV(path), &saved) == AppSettingsLoadResult::Success);
    CHECK_ALL(saved.PermissionReviewerThreshold == 0.8,
              saved.PermissionMode == AgentPermissionMode::Ask,
              rt.PermissionModeOverridden,
              AgentHarness_GetPermissionMode(&harness) == AgentPermissionMode::Auto,
              AgentHarness_PermissionGrantCount(&harness) == 1);
    rt.PermissionModeOverridden = false;
    rt.PersistedPermissionMode = AgentPermissionMode::Auto;
  }

  {
    struct ModelCase {
      StrView Command;
      StrView Model;
    };
    const ModelCase valid[] = {
        {SVL("/permissions model jev-latest"), SVL("jev-latest")},
        {SVL("/permissions model   jev-preview  "), SVL("jev-preview")},
        {SVL("/permissions model jev-1.13.0"), SVL("jev-1.13.0")}};
    AppSettings saved = {};
    for (const ModelCase& test : valid) {
      u32 errors = capture.Errors;
      i32 revision = Atomic_LoadI32(&harness.Permissions.Revision);
      void* reviewer = harness.Config.PermissionReviewer.User;
      char input[96];
      memcpy(input, test.Command.Data, test.Command.Length);
      RuntimeDispatchCommand(&rt, StrView{input, test.Command.Length}, &handlers, &ux);
      memset(input, 'x', sizeof(input));
      CHECK_ALL(capture.Errors == errors,
                StrView_Contains(capture.LastNotice, "model saved and set to"),
                StrView_Eq(rt.PermissionReviewerModel, test.Model),
                StrView_Eq(harness.Config.PermissionReviewerModel, test.Model),
                StrView_Eq(harness.Config.ModelInfo.Id, model.Id),
                rt.PermissionReviewerThreshold == 0.8,
                harness.Config.PermissionReviewerThreshold == 0.8,
                harness.Config.PermissionReviewer.User != reviewer,
                Atomic_LoadI32(&harness.Permissions.Revision) == revision + 1,
                AgentHarness_PermissionGrantCount(&harness) == 1,
                AgentHarness_GetPermissionMode(&harness) == AgentPermissionMode::Auto);
      CHECK(AppSettings_Load(&arena, SV(path), &saved) == AppSettingsLoadResult::Success);
      CHECK_ALL(StrView_Eq(saved.PermissionReviewerModel, test.Model),
                StrView_Eq(saved.Model, model.Id),
                saved.PermissionReviewerThreshold == 0.8,
                saved.PermissionMode == AgentPermissionMode::Auto);
      RuntimeDispatchCommand(&rt, SVL("/permissions model"), &handlers, &ux);
      CHECK_ALL(StrView_Contains(capture.LastNotice, test.Model.Data),
                Atomic_LoadI32(&harness.Permissions.Revision) == revision + 1);
    }

    i32 revision = Atomic_LoadI32(&harness.Permissions.Revision);
    StrView previousModel = rt.PermissionReviewerModel;
    void* reviewer = harness.Config.PermissionReviewer.User;
    const StrView invalidModels[] = {
        SVL("/permissions model z-ai/glm-5.2"),
        SVL("/permissions model openrouter/auto"),
        SVL("/permissions model null"),
        SVL("/permissions model jev-"),
        SVL("/permissions model JEV-1.13.0"),
        SVL("/permissions model jev-latest extra"),
        SVL("/permissions model jev-1.13.0/other"),
        SVL("/permissions modelx jev-latest")};
    for (StrView command : invalidModels) {
      u32 errors = capture.Errors;
      RuntimeDispatchCommand(&rt, command, &handlers, &ux);
      CHECK_ALL(capture.Errors == errors + 1,
                StrView_Contains(capture.LastError, "Usage: /permissions"),
                rt.PermissionReviewerModel.Data == previousModel.Data,
                harness.Config.PermissionReviewer.User == reviewer,
                Atomic_LoadI32(&harness.Permissions.Revision) == revision);
    }

    harness.Running = true;
    u32 errors = capture.Errors;
    RuntimeDispatchCommand(&rt, SVL("/permissions model"), &handlers, &ux);
    CHECK_ALL(capture.Errors == errors,
              StrView_Contains(capture.LastNotice, "Permission reviewer model: jev-1.13.0"));
    RuntimeDispatchCommand(&rt, SVL("/permissions model jev-latest"), &handlers, &ux);
    harness.Running = false;
    CHECK_ALL(capture.Errors == errors + 1,
              StrView_Contains(capture.LastError, "idle"),
              rt.PermissionReviewerModel.Data == previousModel.Data,
              harness.Config.PermissionReviewer.User == reviewer,
              Atomic_LoadI32(&harness.Permissions.Revision) == revision);

    rt.SettingsPath = Fs_Join(&arena, SV(path), SVL("not-a-directory.json"));
    errors = capture.Errors;
    RuntimeDispatchCommand(&rt, SVL("/permissions model jev-latest"), &handlers, &ux);
    rt.SettingsPath = SV(path);
    CHECK_ALL(capture.Errors == errors + 1,
              StrView_Contains(capture.LastError, "was not changed"),
              rt.PermissionReviewerModel.Data == previousModel.Data,
              StrView_Eq(harness.Config.PermissionReviewerModel, previousModel),
              harness.Config.PermissionReviewer.User == reviewer,
              Atomic_LoadI32(&harness.Permissions.Revision) == revision);
    CHECK(AppSettings_Load(&arena, SV(path), &saved) == AppSettingsLoadResult::Success);
    CHECK(StrView_Eq(saved.PermissionReviewerModel, previousModel));

    rt.PermissionModeOverridden = true;
    rt.PersistedPermissionMode = AgentPermissionMode::Ask;
    RuntimeDispatchCommand(&rt, SVL("/permissions model jev-latest"), &handlers, &ux);
    CHECK(AppSettings_Load(&arena, SV(path), &saved) == AppSettingsLoadResult::Success);
    CHECK_ALL(StrView_EqCStr(saved.PermissionReviewerModel, "jev-latest"),
              saved.PermissionMode == AgentPermissionMode::Ask,
              rt.PermissionModeOverridden,
              AgentHarness_GetPermissionMode(&harness) == AgentPermissionMode::Auto,
              AgentHarness_PermissionGrantCount(&harness) == 1);
    RuntimeDispatchCommand(&rt, SVL("/permissions"), &handlers, &ux);
    CHECK(StrView_Contains(capture.LastNotice, "Permission reviewer model: jev-latest"));
    rt.PermissionModeOverridden = false;
    rt.PersistedPermissionMode = AgentPermissionMode::Auto;
  }

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
  InteractiveRuntime rt = {};
  InteractiveRuntime_Init(&rt, &arena);
  AgentHarness harness = {};
  rt.Harness = &harness;
  rt.PermissionReviewerModel = {};
  CHECK(RuntimeRefreshPermissionReviewer(&rt));
  CHECK_ALL(rt.Spec == nullptr,
            StrView_Eq(harness.Config.PermissionReviewerModel,
                       AGENT_PERMISSION_DEFAULT_REVIEWER_MODEL),
            harness.Config.PermissionReviewer.Review != nullptr,
            harness.Config.PermissionReviewer.User != nullptr,
            harness.Config.PermissionReviewerThreshold == 0.95);
  AgentPermissionReviewFn review = harness.Config.PermissionReviewer.Review;
  AgentPermissionEngine_Init(&harness.Permissions, AgentPermissionMode::Auto,
                              {}, nullptr, harness.Config.PermissionReviewer);
  i32 revision = Atomic_LoadI32(&harness.Permissions.Revision);

  ProviderSpec provider = {};
  provider.Id = SVL("chat-provider-without-model-factory");
  rt.Spec = &provider;
  rt.PermissionReviewerModel = SVL("jev-1.12.0");
  CHECK(RuntimeRefreshPermissionReviewer(&rt));
  CHECK_ALL(StrView_EqCStr(harness.Config.PermissionReviewerModel,
                           "jev-1.12.0"),
            harness.Config.PermissionReviewer.Review == review,
            Atomic_LoadI32(&harness.Permissions.Revision) == revision + 1);
  revision = Atomic_LoadI32(&harness.Permissions.Revision);
  rt.PermissionReviewerThreshold = 0.9;
  CHECK(RuntimeRefreshPermissionReviewer(&rt));
  CHECK_ALL(Atomic_LoadI32(&harness.Permissions.Revision) == revision + 1,
            harness.Config.PermissionReviewerThreshold == 0.9,
            AgentPermissionEngine_GetMode(&harness.Permissions) ==
                AgentPermissionMode::Auto);
  revision = Atomic_LoadI32(&harness.Permissions.Revision);

  AgentPermissionReviewer previous = harness.Config.PermissionReviewer;
  rt.PermissionReviewerModel = SVL("jev-");
  CHECK(!RuntimeRefreshPermissionReviewer(&rt));
  CHECK_ALL(StrView_EqCStr(harness.Config.PermissionReviewerModel,
                           "jev-1.12.0"),
            harness.Config.PermissionReviewer.Review == previous.Review,
            harness.Config.PermissionReviewer.User == previous.User,
            Atomic_LoadI32(&harness.Permissions.Revision) == revision);
  rt.PermissionReviewerModel = SVL("z-ai/glm-5.2");
  CHECK(!RuntimeRefreshPermissionReviewer(&rt));
  rt.PermissionReviewerModel = SVL("jev-1.12.0");
  rt.PermissionReviewerThreshold = 1.1;
  CHECK(!RuntimeRefreshPermissionReviewer(&rt));
  CHECK_ALL(harness.Config.PermissionReviewerThreshold == 0.9,
            harness.Config.PermissionReviewer.User == previous.User,
            Atomic_LoadI32(&harness.Permissions.Revision) == revision);

  AgentPermissionEngine_Close(&harness.Permissions);
  InteractiveRuntime_Release(&rt);
  ArenaRelease(&arena);
}

void TestRuntimeReviewerProviderIndependence() {
  Arena arena = ArenaCreate();
  const char* dir = "clanker_reviewer_provider_test";
  const char* path = "clanker_reviewer_provider_test/settings.json";
  remove(path);

  ProviderSpec first = {};
  first.Id = SVL("provider-one");
  first.Name = SVL("Provider One");
  first.DefaultModel = SVL("chat-one");
  first.CreateProvider = TestChatProvider;
  ProviderSpec second = first;
  second.Id = SVL("provider-two");
  second.Name = SVL("Provider Two");
  second.DefaultModel = SVL("chat-two");

  MemoryRepository memory = {};
  MemoryRepository_Init(&memory);
  SessionRepository sessions = MemoryRepository_AsRepository(&memory);
  SessionCreateOptions create = {};
  create.ProjectKey = SVL("reviewer-provider-test");
  create.WorkingDirectory = Sys_CurrentDir(&arena);
  SessionLog log = {};
  SessionInfo info = {};
  CHECK(SessionResult_IsOk(
      sessions.Create(sessions.Ctx, &arena, &create, &log, &info)));

  Model firstModel = TestChatModel(&arena, &first, first.DefaultModel);
  AgentHarnessConfig config = {};
  config.ModelInfo = firstModel;
  config.ProviderImpl = TestChatProvider(&arena, &first);
  config.ResolveAuth = NoRuntimeAuth;
  config.PermissionReviewerModel = SVL("jev-1.12.0");
  config.PermissionReviewerThreshold = 0.85;
  config.PermissionReviewer =
      TypeSafePermissionReviewer_Make(&arena, config.PermissionReviewerModel,
                                      config.PermissionReviewerThreshold);
  config.WorkingDirectory = create.WorkingDirectory;
  AgentPermissionReviewer reviewer = config.PermissionReviewer;
  AgentHarness harness = {};
  CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));

  InteractiveRuntime rt = {};
  InteractiveRuntime_Init(&rt, &arena);
  rt.Spec = &first;
  rt.CurrentModel = firstModel;
  rt.SettingsPath = SV(path);
  rt.Harness = &harness;
  rt.Sessions = &sessions;
  rt.ProjectKey = create.ProjectKey;
  rt.WorkingDirectory = create.WorkingDirectory;
  rt.PermissionReviewerModel = config.PermissionReviewerModel;
  rt.PermissionReviewerThreshold = config.PermissionReviewerThreshold;
  AppCommandUx ux = {};
  Model secondModel = TestChatModel(&arena, &second, second.DefaultModel);
  CHECK(ActivateRuntimeSelection(&rt, &second, secondModel, {}, &ux));
  CHECK_ALL(rt.Spec == &second,
            harness.Config.PermissionReviewerThreshold == 0.85,
            StrView_EqCStr(harness.Config.ModelInfo.Id, "chat-two"),
            StrView_EqCStr(harness.Config.PermissionReviewerModel, "jev-1.12.0"),
            harness.Config.PermissionReviewer.Review == reviewer.Review,
            harness.Config.PermissionReviewer.User == reviewer.User);

  AppSettings loaded = {};
  CHECK(AppSettings_Load(&arena, SV(path), &loaded) ==
        AppSettingsLoadResult::Success);
  CHECK_ALL(StrView_Eq(loaded.Provider, second.Id),
            loaded.PermissionReviewerThreshold == 0.85,
            StrView_EqCStr(loaded.PermissionReviewerModel, "jev-1.12.0"));

  SessionLog restoredLog = {};
  SessionInfo restoredInfo = {};
  CHECK(SessionResult_IsOk(sessions.Create(sessions.Ctx, &arena, &create,
                                           &restoredLog, &restoredInfo)));
  restoredInfo.CurrentProvider = ProviderRegistry_At(0)->Id;
  restoredInfo.CurrentModel = SVL("restored-chat");
  CHECK(SessionResult_IsOk(
      RuntimeSwitchToLog(&rt, restoredLog, &restoredInfo, &ux)));
  CHECK_ALL(rt.Spec == ProviderRegistry_At(0),
            harness.Config.PermissionReviewerThreshold == 0.85,
            StrView_EqCStr(harness.Config.ModelInfo.Id, "restored-chat"),
            StrView_EqCStr(harness.Config.PermissionReviewerModel, "jev-1.12.0"),
            harness.Config.PermissionReviewer.Review == reviewer.Review,
            harness.Config.PermissionReviewer.User == reviewer.User);
  CHECK(RuntimeResetSession(&rt, &ux));
  CHECK_ALL(StrView_EqCStr(harness.Config.PermissionReviewerModel, "jev-1.12.0"),
            harness.Config.PermissionReviewerThreshold == 0.85,
            harness.Config.PermissionReviewer.Review == reviewer.Review,
            harness.Config.PermissionReviewer.User == reviewer.User);

  // A rebuilt harness can also install a missing callback without borrowing
  // the closing harness's arenas.
  harness.Config.PermissionReviewer = {};
  CHECK(RuntimeResetSession(&rt, &ux));
  CHECK_ALL(StrView_EqCStr(harness.Config.PermissionReviewerModel, "jev-1.12.0"),
            harness.Config.PermissionReviewer.Review == reviewer.Review,
            harness.Config.PermissionReviewer.User != nullptr);
  void* previousReviewer = harness.Config.PermissionReviewer.User;
  rt.PermissionReviewerThreshold = 0.99;
  CHECK(RuntimeResetSession(&rt, &ux));
  CHECK_ALL(harness.Config.PermissionReviewerThreshold == 0.99,
            harness.Config.PermissionReviewer.User != previousReviewer);

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

void TestRuntimeReviewerReload() {
  Arena arena = ArenaCreate();
  const char* dir = "clanker_reviewer_reload_test";
  const char* path = "clanker_reviewer_reload_test/settings.json";
  remove(path);
  CHECK(ConfigFile_WriteAtomic(
      &arena, SV(path),
      SVL("{\"provider\":\"openrouter\",\"model\":\"settings-chat\","
          "\"permissionMode\":\"ask\","
          "\"permissionReviewerThreshold\":0.82,"
          "\"permissionReviewerModel\":\"jev-latest\"}")));

  MemoryRepository memory = {};
  MemoryRepository_Init(&memory);
  SessionRepository sessions = MemoryRepository_AsRepository(&memory);
  SessionCreateOptions create = {};
  create.ProjectKey = SVL("reviewer-reload-test");
  create.WorkingDirectory = Fs_RealPath(&arena, SV(dir));
  SessionLog log = {};
  SessionInfo info = {};
  CHECK(SessionResult_IsOk(
      sessions.Create(sessions.Ctx, &arena, &create, &log, &info)));
  const ProviderSpec* spec = ProviderRegistry_At(0);
  Model chatModel = TestChatModel(&arena, spec, SVL("active-chat"));
  AgentHarnessConfig config = {};
  config.ModelInfo = chatModel;
  config.ProviderImpl = TestChatProvider(&arena, spec);
  config.PermissionReviewerModel = SVL("jev-1.12.0");
  config.PermissionReviewer =
      TypeSafePermissionReviewer_Make(&arena, config.PermissionReviewerModel);
  config.WorkingDirectory = create.WorkingDirectory;
  AgentHarness harness = {};
  CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));

  InteractiveRuntime rt = {};
  InteractiveRuntime_Init(&rt, &arena);
  rt.SettingsPath = SV(path);
  rt.CurrentModel = chatModel;
  rt.Harness = &harness;
  rt.WorkingDirectory = create.WorkingDirectory;
  rt.AgentDirectory = create.WorkingDirectory;
  rt.TrustOverride = ProjectTrustOverride::Untrusted;
  rt.PermissionReviewerModel = config.PermissionReviewerModel;
  PromptContext prompt = {};
  rt.Prompt = &prompt;
  PromptContextOptions options = {};
  options.WorkingDirectory = rt.WorkingDirectory;
  options.AgentDirectory = rt.AgentDirectory;
  options.NoContextFiles = true;
  options.NoSkills = true;
  options.NoPromptTemplates = true;
  options.HasExplicitSystemPrompt = true;
  options.ExplicitSystemPrompt = SVL("Reviewer reload test.");
  RuntimeSetPromptOptions(&rt, &options);
  RuntimeCommandCapture capture = {};
  AppCommandUx ux = {&capture, CaptureRuntimeNotice, CaptureRuntimeError};
  AppCommandHandlers handlers = RuntimeCommandHandlers(&rt);
  RuntimeDispatchCommand(&rt, SVL("/reload"), &handlers, &ux);
  CHECK_ALL(capture.Errors == 0,
            rt.PermissionReviewerThreshold == 0.82,
            harness.Config.PermissionReviewerThreshold == 0.82,
            rt.Spec == nullptr,
            StrView_EqCStr(harness.Config.ModelInfo.Id, "active-chat"),
            harness.Config.ProviderImpl.Stream == UnexpectedChatStream,
            harness.Config.PermissionReviewer.Review ==
                config.PermissionReviewer.Review,
            StrView_EqCStr(harness.Config.PermissionReviewerModel, "jev-latest"),
            AgentHarness_GetPermissionMode(&harness) == AgentPermissionMode::Ask);

  RuntimeDispatchCommand(&rt, SVL("/permissions"), &handlers, &ux);
  CHECK(StrView_Contains(capture.LastNotice, "Permission reviewer threshold: 0.82"));
  i32 revision = Atomic_LoadI32(&harness.Permissions.Revision);
  CHECK(ConfigFile_WriteAtomic(
      &arena, SV(path),
      SVL("{\"provider\":\"openrouter\",\"model\":\"settings-chat\","
          "\"permissionMode\":\"ask\",\"permissionReviewerThreshold\":0.99}")));
  RuntimeDispatchCommand(&rt, SVL("/reload"), &handlers, &ux);
  CHECK_ALL(capture.Errors == 0, rt.PermissionReviewerThreshold == 0.99,
            harness.Config.PermissionReviewerThreshold == 0.99,
            Atomic_LoadI32(&harness.Permissions.Revision) > revision);

  AgentPermissionReviewer reviewer = harness.Config.PermissionReviewer;
  StrView previousPrompt = prompt.SystemPrompt;
  u32 notices = capture.Notices;
  revision = Atomic_LoadI32(&harness.Permissions.Revision);
  const char* invalidReviewers[] = {"jev-", "z-ai/glm-5.2"};
  for (u32 i = 0; i < sizeof(invalidReviewers) / sizeof(invalidReviewers[0]); i++) {
    StrBuf content = StrBuf_Make(&arena, 160);
    StrBuf_AppendCStr(
        &content,
        "{\"provider\":\"openrouter\",\"model\":\"settings-chat\","
        "\"permissionMode\":\"yolo\",\"permissionReviewerThreshold\":0.5,"
        "\"permissionReviewerModel\":\"");
    StrBuf_AppendCStr(&content, invalidReviewers[i]);
    StrBuf_AppendCStr(&content, "\"}");
    CHECK(ConfigFile_WriteAtomic(&arena, SV(path), StrBuf_View(&content)));
    RuntimeDispatchCommand(&rt, SVL("/reload"), &handlers, &ux);
    CHECK_ALL(capture.Errors == i + 1, capture.Notices == notices,
              StrView_Contains(capture.LastError, "Settings are malformed"),
              prompt.SystemPrompt.Data == previousPrompt.Data,
              harness.Config.PermissionReviewer.User == reviewer.User,
              StrView_Eq(harness.Config.PermissionReviewerModel,
                         AGENT_PERMISSION_DEFAULT_REVIEWER_MODEL),
              StrView_Eq(rt.PermissionReviewerModel,
                         AGENT_PERMISSION_DEFAULT_REVIEWER_MODEL),
              rt.PermissionReviewerThreshold == 0.99,
              harness.Config.PermissionReviewerThreshold == 0.99,
              AgentHarness_GetPermissionMode(&harness) == AgentPermissionMode::Ask,
              Atomic_LoadI32(&harness.Permissions.Revision) == revision);
  }

  CHECK(ConfigFile_WriteAtomic(
      &arena, SV(path),
      SVL("{\"provider\":\"openrouter\",\"model\":\"settings-chat\","
          "\"permissionReviewerThreshold\":1.1}")));
  RuntimeDispatchCommand(&rt, SVL("/reload"), &handlers, &ux);
  CHECK_ALL(capture.Errors == 3,
            prompt.SystemPrompt.Data == previousPrompt.Data,
            harness.Config.PermissionReviewer.User == reviewer.User,
            rt.PermissionReviewerThreshold == 0.99,
            harness.Config.PermissionReviewerThreshold == 0.99);

  rt.Spec = spec;
  CHECK(RuntimePersistSettings(&rt));
  RuntimeDispatchCommand(&rt, SVL("/reload"), &handlers, &ux);
  CHECK_ALL(capture.Errors == 3, rt.PermissionReviewerThreshold == 0.99,
            StrView_Eq(harness.Config.PermissionReviewerModel,
                       AGENT_PERMISSION_DEFAULT_REVIEWER_MODEL));

  PromptContext_Release(&prompt);
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
            defaults.Settings.PermissionMode == AgentPermissionMode::Auto,
            StrView_Eq(defaults.Settings.PermissionReviewerModel,
                       AGENT_PERMISSION_DEFAULT_REVIEWER_MODEL),
            !defaults.Settings.PermissionReviewerInvalid,
            defaults.Settings.PermissionReviewerThreshold == 0.95,
            !defaults.Settings.PermissionReviewerThresholdInvalid,
            !defaults.Settings.YoloAcknowledged);

  CHECK(ConfigFile_WriteAtomic(
      &arena, SV(path),
      SVL("{\"provider\":\"openrouter\",\"model\":\"configured\","
          "\"permissionMode\":\"yolo\","
          "\"permissionReviewerModel\":\"jev-1.12.0\","
          "\"permissionReviewerThreshold\":0.85,"
          "\"yoloAcknowledged\":true}")));
  RuntimeSelection configured = LoadRuntimeSelection(&arena, SV(path), "override");
  CHECK_ALL(StrView_EqCStr(configured.ModelId, "override"),
            configured.Settings.PermissionReviewerThreshold == 0.85,
            configured.Settings.PermissionMode == AgentPermissionMode::Yolo,
            StrView_EqCStr(configured.Settings.PermissionReviewerModel,
                           "jev-1.12.0"),
            !configured.Settings.PermissionReviewerInvalid,
            configured.Settings.YoloAcknowledged);

  CHECK(ConfigFile_WriteAtomic(
      &arena, SV(path),
      SVL("{\"provider\":\"openrouter\",\"model\":\"configured\","
          "\"permissionMode\":\"ask\","
          "\"permissionReviewerModel\":\"z-ai/glm-5.2\"}")));
  RuntimeSelection unsupported = LoadRuntimeSelection(&arena, SV(path), "override");
  CHECK_ALL(unsupported.Settings.PermissionReviewerInvalid,
            StrView_EqCStr(unsupported.ModelId, "override"));

  CHECK(ConfigFile_WriteAtomic(
      &arena, SV(path),
      SVL("{\"provider\":\"openrouter\",\"model\":\"configured\","
          "\"permissionReviewerModel\":\"jev-\"}")));
  RuntimeSelection invalid = LoadRuntimeSelection(&arena, SV(path), nullptr);
  CHECK(invalid.Settings.PermissionReviewerInvalid);

  CHECK(ConfigFile_WriteAtomic(
      &arena, SV(path),
      SVL("{\"provider\":\"openrouter\",\"model\":\"configured\","
          "\"permissionReviewerThreshold\":-1}")));
  RuntimeSelection invalidThreshold = LoadRuntimeSelection(&arena, SV(path), nullptr);
  CHECK_ALL(invalidThreshold.Settings.PermissionReviewerThresholdInvalid,
            !invalidThreshold.Settings.PermissionReviewerInvalid);

  CHECK(ConfigFile_WriteAtomic(&arena, SV(path), SVL("{bad")));
  RuntimeSelection malformed = LoadRuntimeSelection(&arena, SV(path), nullptr);
  CHECK_ALL(malformed.Spec == ProviderRegistry_At(0),
            StrView_Eq(malformed.ModelId, malformed.Spec->DefaultModel),
            malformed.Settings.PermissionMode == AgentPermissionMode::Auto,
            StrView_Eq(malformed.Settings.PermissionReviewerModel,
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

namespace {

struct SecretCommandRun {
  InteractiveRuntime* Runtime;
  AppCommandUx Ux;
  StrView Command;
  AppCommandHandlerResult EntryResult;
  bool DirectEntry;
};

void RunSecretCommand(void* user) {
  SecretCommandRun* run = (SecretCommandRun*)user;
  if (run->DirectEntry) {
    run->EntryResult = RuntimeEnterApiKey(run->Runtime, SVL("synthetic-provider"),
        SVL("Synthetic provider API key"), SVL("Authentication"), &run->Ux);
  } else {
    AppCommandHandlers handlers = RuntimeCommandHandlers(run->Runtime);
    RuntimeDispatchCommand(run->Runtime, run->Command, &handlers, &run->Ux);
  }
}

bool WaitSettingsModal(Tui* tui, TuiModalKind kind, StrView title,
                       TextInputMode mode = TextInputMode::Plain,
                       StrView initial = {}) {
  for (u32 i = 0; i < 5000; i++) {
    Mutex_Lock(&tui->Lock);
    bool ready = tui->ModalKind == kind &&
                 tui->ModalResult == TuiModalResult::Pending &&
                 StrView_Eq(tui->Sess.ModalTitle, title);
    if (ready && kind == TuiModalKind::Text) {
      CHECK(tui->ModalInputMode == mode);
      CHECK(StrView_Eq(LineEdit_Text(&tui->ModalEdit), initial));
    }
    Mutex_Unlock(&tui->Lock);
    if (ready) return true;
    Sleep_Ms(1);
  }
  CHECK(false);
  return false;
}

void FeedSettings(Tui* tui, const char* text) {
  Tui_FeedInput(tui, text, (u32)strlen(text));
}

bool ChooseSettingsItem(Tui* tui, u32 selected) {
  if (!WaitSettingsModal(tui, TuiModalKind::Select, SVL("Permission settings")))
    return false;
  FeedSettings(tui, "\x1b[H");
  for (u32 i = 0; i < selected; i++) FeedSettings(tui, "\x1b[B");
  FeedSettings(tui, "\r");
  return true;
}

void StopSettingsWorker(Tui* tui, Thread* thread) {
  Mutex_Lock(&tui->Lock);
  tui->Stop = true;
  Cond_Broadcast(&tui->ModalCv);
  Mutex_Unlock(&tui->Lock);
  Thread_Join(thread);
}

bool ArenaContainsSyntheticKey(const Arena* arena) {
  return StrView_Contains(StrView{(const char*)arena->Base, (u32)arena->Used},
                          "synthetic-private-key");
}

void TestSecretSettingsCommands() {
  Arena arena = ArenaCreate();
  Arena fixture = ArenaCreate();
  Arena tuiArena = ArenaCreate();
  const char* authPath = "clanker_secret_settings_auth_test.json";
  const char* settingsPath = "clanker_secret_settings_config_test.json";
  remove(authPath);
  remove(settingsPath);
  Credential key = {};
  key.ApiKey = SVL("synthetic-private-key-original");
  CHECK(AuthStore_Write(&fixture, SV(authPath), SVL("typesafe"), &key));
  CHECK(AuthStore_Write(&fixture, SV(authPath), SVL("synthetic-provider"), &key));

  MemoryRepository memory = {};
  MemoryRepository_Init(&memory);
  SessionRepository sessions = MemoryRepository_AsRepository(&memory);
  SessionCreateOptions create = {};
  create.ProjectKey = SVL("secret-settings-test");
  create.WorkingDirectory = SVL("C:\\synthetic");
  SessionLog log = {};
  SessionInfo info = {};
  CHECK(SessionResult_IsOk(sessions.Create(sessions.Ctx, &arena, &create, &log, &info)));
  const ProviderSpec* spec = ProviderRegistry_At(0);
  Model model = spec->CreateModel(&arena, spec, spec->DefaultModel);
  AgentHarnessConfig config = {};
  config.ModelInfo = model;
  config.ProviderImpl = TestChatProvider(&arena, spec);
  config.PermissionReviewerModel = SVL("jev-latest");
  config.PermissionReviewerThreshold = 0.95;
  AgentHarness harness = {};
  CHECK(SessionResult_IsOk(AgentHarness_Init(&harness, log, &config)));
  SessionReduction before = {};
  CHECK(SessionResult_IsOk(AgentHarness_Snapshot(&fixture, &harness, &before)));

  Tui tui = {};
  TuiOpts options = {};
  options.ModelName = model.Id;
  Tui_InitModel(&tui, &tuiArena, options);
  InteractiveRuntime rt = {};
  InteractiveRuntime_Init(&rt, &arena);
  rt.AuthPath = SV(authPath);
  rt.SettingsPath = SV(settingsPath);
  rt.Spec = spec;
  rt.CurrentModel = model;
  rt.Harness = &harness;
  rt.Sessions = &sessions;
  rt.TuiView = &tui;
  RuntimeCommandCapture capture = {};
  AppCommandUx ux = {&capture, CaptureRuntimeNotice, CaptureRuntimeError};
  SecretCommandRun run = {&rt, ux, SVL("/permissions key"), {}, false};
  Thread worker = {};
  i32 revision = Atomic_LoadI32(&harness.Permissions.Revision);
  CHECK(Thread_Start(&worker, RunSecretCommand, &run));
  bool ready = WaitSettingsModal(&tui, TuiModalKind::Text, SVL("Jev API key"),
                                 TextInputMode::Secret);
  if (ready) {
    FeedSettings(&tui, "synthetic-private-key-updated\r");
    Thread_Join(&worker);
  } else {
    StopSettingsWorker(&tui, &worker);
  }
  CHECK_ALL(capture.Errors == 0, capture.Notices == 1,
            Atomic_LoadI32(&harness.Permissions.Revision) == revision + 1,
            !StrView_Contains(capture.LastNotice, "synthetic-private-key"));
  Credential read = {};
  CHECK(AuthStore_Read(&fixture, SV(authPath), SVL("typesafe"), &read));
  CHECK(StrView_EqCStr(read.ApiKey, "synthetic-private-key-updated"));
  CHECK(!ArenaContainsSyntheticKey(&arena));
  CHECK(!ArenaContainsSyntheticKey(&rt.CommandMem));
  CHECK(!ArenaContainsSyntheticKey(&tui.SessionMem));
  CHECK_ALL(rt.PendingPrompt.Length == 0, tui.Edit.History.Count == 0,
            tui.ModalEdit.History.Count == 0, LineEdit_Empty(&tui.ModalEdit));

  revision = Atomic_LoadI32(&harness.Permissions.Revision);
  CHECK(Thread_Start(&worker, RunSecretCommand, &run));
  ready = WaitSettingsModal(&tui, TuiModalKind::Text, SVL("Jev API key"),
                            TextInputMode::Secret);
  if (ready) {
    FeedSettings(&tui, "synthetic-private-key-cancelled");
    FeedSettings(&tui, "\x1b");
    Thread_Join(&worker);
  } else {
    StopSettingsWorker(&tui, &worker);
  }
  CHECK(Atomic_LoadI32(&harness.Permissions.Revision) == revision);
  CHECK(AuthStore_Read(&fixture, SV(authPath), SVL("typesafe"), &read));
  CHECK(StrView_EqCStr(read.ApiKey, "synthetic-private-key-updated"));
  CHECK(!StrView_Contains(capture.LastNotice, "synthetic-private-key"));

  StrView preserved = SVL(
      "{\"typesafe\":{\"type\":\"api_key\",\"key\":\"synthetic-private-key-updated\"},"
      "\"other\":false}");
  CHECK(ConfigFile_WriteAtomic(&fixture, SV(authPath), preserved));
  CHECK(Thread_Start(&worker, RunSecretCommand, &run));
  ready = WaitSettingsModal(&tui, TuiModalKind::Text, SVL("Jev API key"),
                            TextInputMode::Secret);
  if (ready) {
    FeedSettings(&tui, "synthetic-private-key-rejected\r");
    Thread_Join(&worker);
  } else {
    StopSettingsWorker(&tui, &worker);
  }
  CHECK_ALL(capture.Errors == 1,
            !StrView_Contains(capture.LastError, "synthetic-private-key"),
            Atomic_LoadI32(&harness.Permissions.Revision) == revision);
  StrView unchanged = {};
  CHECK(ConfigFile_ReadAll(&fixture, SV(authPath), &unchanged) == ConfigFileReadResult::Success);
  CHECK(StrView_Eq(unchanged, preserved));
  CHECK(AuthStore_Read(&fixture, SV(authPath), SVL("typesafe"), &read));
  CHECK(StrView_EqCStr(read.ApiKey, "synthetic-private-key-updated"));


  // `/auth` signs in through OpenRouter's browser flow; the masked entry it
  // used to open is still how other credentials are entered.
  remove(authPath);
  CHECK(AuthStore_Write(&fixture, SV(authPath), SVL("typesafe"), &key));
  run.DirectEntry = true;
  CHECK(Thread_Start(&worker, RunSecretCommand, &run));
  ready = WaitSettingsModal(&tui, TuiModalKind::Text, SVL("Synthetic provider API key"),
                            TextInputMode::Secret);
  if (ready) {
    FeedSettings(&tui, "synthetic-private-key-provider\r");
    Thread_Join(&worker);
  } else {
    StopSettingsWorker(&tui, &worker);
  }
  CHECK(run.EntryResult.Status == AppCommandHandlerStatus::Success);
  CHECK(!StrView_Contains(run.EntryResult.Message, "synthetic-private-key"));
  CHECK(AuthStore_Read(&fixture, SV(authPath), SVL("synthetic-provider"), &read));
  CHECK(StrView_EqCStr(read.ApiKey, "synthetic-private-key-provider"));
  CHECK(AuthStore_Read(&fixture, SV(authPath), SVL("typesafe"), &read));
  CHECK(StrView_Eq(read.ApiKey, key.ApiKey));

  run.DirectEntry = false;
  run.Command = SVL("/permissions");
  capture = {};
  CHECK(Thread_Start(&worker, RunSecretCommand, &run));
  ready = ChooseSettingsItem(&tui, 3) &&
      WaitSettingsModal(&tui, TuiModalKind::Text, SVL("Automatic approval threshold"),
                        TextInputMode::Plain, SVL("0.95"));
  if (ready) {
    FeedSettings(&tui, "\x01\x0binvalid\r");
    ready = WaitSettingsModal(&tui, TuiModalKind::Text,
        SVL("Automatic approval threshold"), TextInputMode::Plain, SVL("0.95"));
  }
  if (ready) {
    FeedSettings(&tui, "\x01\x0b" "0.8\r");
    ready = ChooseSettingsItem(&tui, 2) &&
        WaitSettingsModal(&tui, TuiModalKind::Text, SVL("Jev permission reviewer model"),
                          TextInputMode::Plain, SVL("jev-latest"));
  }
  if (ready) {
    FeedSettings(&tui, "\x01\x0b" "chat-model\r");
    ready = WaitSettingsModal(&tui, TuiModalKind::Text,
        SVL("Jev permission reviewer model"), TextInputMode::Plain, SVL("jev-latest"));
  }
  if (ready) {
    FeedSettings(&tui, "\x01\x0b" "jev-preview\r");
    ready = WaitSettingsModal(&tui, TuiModalKind::Select, SVL("Permission settings"));
  }
  if (ready) {
    FeedSettings(&tui, "\x1b");
    Thread_Join(&worker);
  } else {
    StopSettingsWorker(&tui, &worker);
  }
  CHECK_ALL(capture.Errors == 2, rt.PermissionReviewerThreshold == 0.8,
            StrView_EqCStr(rt.PermissionReviewerModel, "jev-preview"));
  AppSettings saved = {};
  CHECK(AppSettings_Load(&fixture, SV(settingsPath), &saved) == AppSettingsLoadResult::Success);
  CHECK_ALL(saved.PermissionReviewerThreshold == 0.8,
            StrView_EqCStr(saved.PermissionReviewerModel, "jev-preview"));
  StrView settings = {};
  CHECK(ConfigFile_ReadAll(&fixture, SV(settingsPath), &settings) == ConfigFileReadResult::Success);
  CHECK(!StrView_Contains(settings, "synthetic-private-key"));

  AppCommandHandlers handlers = RuntimeCommandHandlers(&rt);
  revision = Atomic_LoadI32(&harness.Permissions.Revision);
  RuntimeDispatchCommand(&rt, SVL("/permissions key synthetic-private-key-inline"), &handlers, &ux);
  CHECK(!StrView_Contains(capture.LastError, "synthetic-private-key"));
  RuntimeDispatchCommand(&rt, SVL("/auth synthetic-private-key-inline"), &handlers, &ux);
  CHECK(!StrView_Contains(capture.LastError, "synthetic-private-key"));
  CHECK_ALL(Atomic_LoadI32(&harness.Permissions.Revision) == revision,
            rt.PendingPrompt.Length == 0);
  RuntimeDispatchCommand(&rt, SVL("/permissions key clear"), &handlers, &ux);
  CHECK(Atomic_LoadI32(&harness.Permissions.Revision) == revision + 1);
  CHECK(!AuthStore_Read(&fixture, SV(authPath), SVL("typesafe"), &read));
  CHECK(AuthStore_Read(&fixture, SV(authPath), SVL("synthetic-provider"), &read));
  CHECK(!StrView_Contains(capture.LastNotice, "synthetic-private-key"));
  SessionReduction after = {};
  CHECK(SessionResult_IsOk(AgentHarness_Snapshot(&fixture, &harness, &after)));
  CHECK_ALL(after.RecordCount == before.RecordCount,
            !ArenaContainsSyntheticKey(&arena),
            !ArenaContainsSyntheticKey(&rt.CommandMem),
            !ArenaContainsSyntheticKey(&tui.SessionMem));

  Tui_Stop(&tui);
  InteractiveRuntime_Release(&rt);
  AgentHarness_Close(&harness);
  MemoryRepository_Destroy(&memory);
  remove(authPath);
  remove(settingsPath);
  ArenaReleaseSecure(&fixture);
  ArenaRelease(&tuiArena);
  ArenaRelease(&arena);
}

void TestPlainTextInputPrivacy() {
  const char* path = "clanker_plain_input_test.txt";
  FILE* input = fopen(path, "w+b");
  CHECK(input != nullptr);
  if (!input) return;
  fputs("unconsumed-input\nabcdefghijk\nnext\n", input);
  fputs("/auth ", input);
  for (u32 i = 0; i < 9000; i++) fputc('x', input);
  fputs("synthetic-private-key-tail\n/permissions model\n/permissions key ", input);
  for (u32 i = 0; i < 9000; i++) fputc('y', input);
  fputs("synthetic-private-key-tail\n/exit\n\n  ordinary prompt text  \n", input);
  fflush(input);
  rewind(input);
#if defined(_WIN32)
  int saved = _dup(_fileno(stdin));
  int redirected = saved < 0 ? -1 : _dup2(_fileno(input), _fileno(stdin));
#else
  int saved = dup(fileno(stdin));
  int redirected = saved < 0 ? -1 : dup2(fileno(input), fileno(stdin));
#endif
  CHECK(redirected >= 0);
  if (redirected >= 0) {
    clearerr(stdin);
    char buffer[64] = "synthetic-private-key";
    StrView text = {};
    bool failed = false;
    CHECK(!Plain_ReadText(SVL("Secret"), buffer, sizeof(buffer), &text,
                          TextInputMode::Secret, &failed));
    CHECK_ALL(failed, text.Length == 0);
    for (u32 i = 0; i < sizeof(buffer); i++) CHECK(buffer[i] == 0);
    CHECK(Plain_ReadText(SVL("Plain"), buffer, sizeof(buffer), &text));
    CHECK(StrView_EqCStr(text, "unconsumed-input"));
    CHECK(!Plain_ReadText(SVL("Bounded"), buffer, 8, &text,
                          TextInputMode::Plain, &failed));
    CHECK_ALL(failed, text.Length == 0);
    CHECK(Plain_ReadText(SVL("Next"), buffer, sizeof(buffer), &text));
    CHECK(StrView_EqCStr(text, "next"));
    char command[8192] = {};
    CHECK(Plain_ReadLine(command, sizeof(command)) == PlainLineResult::TooLong);
    bool wiped = true;
    for (u32 i = 0; i < sizeof(command); i++) wiped = wiped && command[i] == 0;
    CHECK(wiped);
    CHECK(Plain_ReadLine(command, sizeof(command)) == PlainLineResult::Read);
    AppCommand parsed = AppCommand_Parse(SV(command));
    CHECK_ALL(parsed.Kind == AppCommandKind::Permissions,
              StrView_EqCStr(parsed.Arguments, "model"));
    CHECK(Plain_ReadLine(command, sizeof(command)) == PlainLineResult::TooLong);
    CHECK(command[0] == 0);
    CHECK(Plain_ReadLine(command, sizeof(command)) == PlainLineResult::Read);
    CHECK(AppCommand_Parse(SV(command)).Kind == AppCommandKind::Exit);
    CHECK(Plain_ReadLine(command, sizeof(command)) == PlainLineResult::Read);
    CHECK(StrView_Trim(SV(command)).Length == 0);
    CHECK(Plain_ReadLine(command, sizeof(command)) == PlainLineResult::Read);
    CHECK(StrView_EqCStr(SV(command), "  ordinary prompt text  \n"));
    CHECK(Plain_ReadLine(command, sizeof(command)) == PlainLineResult::End);
    CHECK(command[0] == 0);
    Memory_Zero(command, sizeof(command));
    Memory_Zero(buffer, sizeof(buffer));
  }
  if (saved >= 0) {
#if defined(_WIN32)
    _dup2(saved, _fileno(stdin));
    _close(saved);
#else
    dup2(saved, fileno(stdin));
    close(saved);
#endif
    clearerr(stdin);
  }
  fclose(input);
  remove(path);
}

}  // namespace

void TestApp() {
  TestSessionProjectKey();
  TestProjectKeyComparison();
  TestRuntimeCommandArena();
  TestRuntimeSetPromptOptions();
  TestRuntimePersistSettings();
  TestRuntimePermissionCommandEdges();
  TestRuntimeRefreshPermissionReviewer();
  TestRuntimeReviewerProviderIndependence();
  TestRuntimeReviewerReload();
  TestRuntimeSelectionPrecedence();
  TestRuntimeAskUserNonInteractive();
  TestEventPumpTranscriptLifecycle();
  TestJsonOutput();
  TestSecretSettingsCommands();
  TestPlainTextInputPrivacy();
  TestConfigPaths();
  TestTrustChoiceMenu();
  TestCommandResultMapping();
}
