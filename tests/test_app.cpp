// Application-layer logic that used to be unreachable from tests.
//
// Everything here lived inside main.cpp, which has no library target, so none
// of it could be exercised. The behaviour is unchanged; only its address is.
#include <string.h>

#include "app/config_paths.h"
#include "app/runtime.h"
#include "app/session_switch.h"
#include "base/arena.h"
#include "check.h"

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
  // The hash half of the key normalizes separators and case, but the readable
  // basename prefix does not, so the two spellings of one directory still
  // produce different keys. Asserting the behaviour that actually ships rather
  // than the one the hash normalization implies; see the note in the commit
  // that split this out of main.cpp.
  StrView upper = SessionProjectKey(&arena, SVL("C:\\Work\\Clank"));
  StrView lower = SessionProjectKey(&arena, SVL("c:/work/clank"));
  CHECK(!StrView_Eq(upper, lower));
  // They do agree on the hash suffix, which is what the normalization buys.
  CHECK_ALL(upper.Length > 17, lower.Length > 17,
            memcmp(upper.Data + upper.Length - 17,
                   lower.Data + lower.Length - 17, 17) == 0);
#endif

  ArenaRelease(&arena);
}

void TestConfigPaths() {
  Arena arena = ArenaCreate();

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

void TestApp() {
  TestSessionProjectKey();
  TestConfigPaths();
  TestTrustChoiceMenu();
  TestTrimAscii();
  TestCommandResultMapping();
}
