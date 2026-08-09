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
#include "app/provider_registry.h"
#include "app/runtime.h"
#include "app/session_switch.h"
#include "app/settings.h"
#include "base/arena.h"
#include "check.h"
#include "session/metadata.h"

namespace {

void TestSessionProjectKey() {
  Arena arena = ArenaCreate();

  // The key is a filesystem-safe basename plus a hash of the full path, so two
  // directories sharing a basename never collide into one session directory.
  StrView a = SessionProjectKey(&arena, SVL("/home/dev/clank"));
  StrView b = SessionProjectKey(&arena, SVL("/srv/build/clank"));
  CHECK_ALL(!StrView_Eq(a, b), StrView_Contains(a, "clank"),
            StrView_Contains(b, "clank"));

  // Same input, same key -- session directories must be stable across runs.
  CHECK(StrView_Eq(a, SessionProjectKey(&arena, SVL("/home/dev/clank"))));

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
  CHECK(StrView_Eq(SessionProjectKey(&arena, SVL("C:\\Work\\Clank")),
                   SessionProjectKey(&arena, SVL("c:/work/clank"))));
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
  CHECK(SessionProjectKey_Eq(SVL("clank-abc"), SVL("clank-abc")));
  CHECK(!SessionProjectKey_Eq(SVL("clank-abc"), SVL("clank-abd")));
  CHECK(!SessionProjectKey_Eq(SVL("clank-abc"), SVL("clank-ab")));
#if defined(_WIN32)
  CHECK(SessionProjectKey_Eq(SVL("Clank-abc"), SVL("clank-abc")));
  CHECK(SessionProjectKey_Eq(SVL("MyProj-0011"), SVL("myproj-0011")));
#else
  CHECK(!SessionProjectKey_Eq(SVL("Clank-abc"), SVL("clank-abc")));
#endif
}

void TestConfigPaths() {  Arena arena = ArenaCreate();

  // Every per-user file hangs off one root, and the root is a prefix of each.
  StrView root = ClankDataRoot(&arena);
  StrView settings = ClankDataPath(&arena, "settings.json");
  CHECK_ALL(StrView_Contains(root, ".clank"),
            StrView_Contains(settings, ".clank/settings.json"),
            settings.Length > root.Length,
            memcmp(settings.Data, root.Data, root.Length) == 0);

  // An empty name is the root itself rather than a trailing slash.
  CHECK(StrView_Eq(ClankDataRoot(&arena), ClankDataPath(&arena, nullptr)));

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

}  // namespace

// Command results allocate from CommandMem, which a zero-initialized runtime
// does not have. `clank --login` built its runtime by hand and skipped it, so
// the success message trapped in ArenaAlloc after the credentials were already
// written -- InteractiveRuntime_Init is what makes that unforgettable.
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

void TestRuntimePersistSettings() {
  Arena arena = ArenaCreate();
  const char* dir = "clank_runtime_settings_test";
  const char* path = "clank_runtime_settings_test/settings.json";
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
  rt.Rounded = false;
  CHECK(RuntimePersistSettings(&rt));

  AppSettings loaded = {};
  CHECK(AppSettings_Load(&arena, SV(path), &loaded) ==
        AppSettingsLoadResult::Success);
  CHECK(!loaded.Rounded);

  InteractiveRuntime_Release(&rt);
  remove(path);
#if defined(_WIN32)
  _rmdir(dir);
#else
  rmdir(dir);
#endif
  ArenaRelease(&arena);
}

void TestApp() {
  TestSessionProjectKey();
  TestProjectKeyComparison();
  TestRuntimeCommandArena();
  TestRuntimePersistSettings();
  TestConfigPaths();
  TestTrustChoiceMenu();
  TestTrimAscii();
  TestCommandResultMapping();
}
