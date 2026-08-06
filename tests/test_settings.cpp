#include <stdio.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "app/settings.h"
#include "base/arena.h"
#include "base/config_file.h"
#include "base/str.h"
#include "check.h"

void TestSettings() {
  Arena a = ArenaCreate();
  const char* dir = "clank_settings_test";
  const char* path = "clank_settings_test/settings.json";
  remove(path);
#if defined(_WIN32)
  _rmdir(dir);
#else
  rmdir(dir);
#endif

  AppSettings loaded = {};
  CHECK(AppSettings_Load(&a, SV(path), &loaded) == AppSettingsLoadResult::Missing);

  CHECK(ConfigFile_WriteAtomic(&a, SV(path), SVL("{bad")));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) == AppSettingsLoadResult::Malformed);

  CHECK(ConfigFile_WriteAtomic(&a, SV(path), SVL("{\"provider\":\"anthropic\"}")));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) == AppSettingsLoadResult::Malformed);

  CHECK(ConfigFile_WriteAtomic(
      &a, SV(path),
      SVL("{\"provider\":\"anthropic\",\"model\":\"legacy\"}")));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) ==
        AppSettingsLoadResult::Success);
  CHECK_ALL(loaded.DefaultTrust == DefaultProjectTrust::Ask,
            loaded.Profile.Effort == ModelEffort::None,
            loaded.Profile.ContextWindow == 0);
  // An absent compaction block leaves the built-in defaults in place.
  CHECK_ALL(loaded.Compaction.Enabled,
            loaded.Compaction.ReserveTokens ==
                AGENT_COMPACTION_DEFAULT_RESERVE_TOKENS,
            loaded.Compaction.KeepRecentTokens ==
                AGENT_COMPACTION_DEFAULT_KEEP_RECENT_TOKENS);

  // A partial compaction block overrides only the keys it names.
  CHECK(ConfigFile_WriteAtomic(
      &a, SV(path),
      SVL("{\"provider\":\"anthropic\",\"model\":\"legacy\","
          "\"compaction\":{\"enabled\":false}}")));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) ==
        AppSettingsLoadResult::Success);
  CHECK_ALL(!loaded.Compaction.Enabled,
            loaded.Compaction.ReserveTokens ==
                AGENT_COMPACTION_DEFAULT_RESERVE_TOKENS,
            loaded.Compaction.KeepRecentTokens ==
                AGENT_COMPACTION_DEFAULT_KEEP_RECENT_TOKENS);

  // Malformed compaction values are rejected rather than silently defaulted.
  CHECK(ConfigFile_WriteAtomic(
      &a, SV(path),
      SVL("{\"provider\":\"anthropic\",\"model\":\"bad\",\"compaction\":8}")));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) ==
        AppSettingsLoadResult::Malformed);
  CHECK(ConfigFile_WriteAtomic(
      &a, SV(path),
      SVL("{\"provider\":\"anthropic\",\"model\":\"bad\","
          "\"compaction\":{\"enabled\":\"yes\"}}")));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) ==
        AppSettingsLoadResult::Malformed);
  CHECK(ConfigFile_WriteAtomic(
      &a, SV(path),
      SVL("{\"provider\":\"anthropic\",\"model\":\"bad\","
          "\"compaction\":{\"reserveTokens\":0}}")));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) ==
        AppSettingsLoadResult::Malformed);
  CHECK(ConfigFile_WriteAtomic(
      &a, SV(path),
      SVL("{\"provider\":\"anthropic\",\"model\":\"bad\","
          "\"compaction\":{\"keepRecentTokens\":2.5}}")));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) ==
        AppSettingsLoadResult::Malformed);

  CHECK(ConfigFile_WriteAtomic(
      &a, SV(path),
      SVL("{\"provider\":\"anthropic\",\"model\":\"bad\","
          "\"effort\":\"extreme\"}")));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) ==
        AppSettingsLoadResult::Malformed);

  CHECK(ConfigFile_WriteAtomic(
      &a, SV(path),
      SVL("{\"provider\":\"anthropic\",\"model\":\"bad\","
          "\"contextWindow\":1.5}")));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) ==
        AppSettingsLoadResult::Malformed);

  CHECK(ConfigFile_WriteAtomic(
      &a, SV(path),
      SVL("{\"provider\":\"anthropic\",\"model\":\"bad\","
          "\"defaultProjectTrust\":\"sometimes\"}")));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) ==
        AppSettingsLoadResult::Malformed);

  AppSettings saved = {};
  saved.Provider = SVL("anthropic");
  saved.Model = SVL("claude-\"test\"");
  saved.DefaultTrust = DefaultProjectTrust::Always;
  saved.Profile.Effort = ModelEffort::High;
  saved.Profile.ContextWindow = 1000000;
  saved.Compaction.Enabled = true;
  saved.Compaction.ReserveTokens = 4096;
  saved.Compaction.KeepRecentTokens = 8192;
  CHECK(AppSettings_Save(&a, SV(path), &saved));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) == AppSettingsLoadResult::Success);
  CHECK_ALL(StrView_Eq(loaded.Provider, saved.Provider),
            StrView_Eq(loaded.Model, saved.Model),
            loaded.DefaultTrust == DefaultProjectTrust::Always,
  loaded.Profile.Effort == ModelEffort::High,
  loaded.Profile.ContextWindow == 1000000);
  CHECK_ALL(loaded.Compaction.Enabled, loaded.Compaction.ReserveTokens == 4096,
            loaded.Compaction.KeepRecentTokens == 8192);

  // A disabled policy round-trips as disabled rather than reverting to the
  // enabled default.
  saved.Compaction.Enabled = false;
  CHECK(AppSettings_Save(&a, SV(path), &saved));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) == AppSettingsLoadResult::Success);
  CHECK_ALL(!loaded.Compaction.Enabled, loaded.Compaction.ReserveTokens == 4096,
            loaded.Compaction.KeepRecentTokens == 8192);

  // A zero-initialized policy must still round-trip into a loadable file.
  AppSettings zeroed = saved;
  zeroed.Compaction = AgentCompactionPolicy{};
  CHECK(AppSettings_Save(&a, SV(path), &zeroed));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) == AppSettingsLoadResult::Success);
  CHECK_ALL(!loaded.Compaction.Enabled,
            loaded.Compaction.ReserveTokens ==
                AGENT_COMPACTION_DEFAULT_RESERVE_TOKENS,
            loaded.Compaction.KeepRecentTokens ==
                AGENT_COMPACTION_DEFAULT_KEEP_RECENT_TOKENS);

#if !defined(_WIN32)
  struct stat st = {};
  CHECK(stat(path, &st) == 0);
  CHECK((st.st_mode & 0777) == 0600);
#endif

  CHECK_ALL(!AppSettings_Save(&a, SV(dir), &saved),
            AppSettings_Load(&a, SV(dir), &loaded) ==
                AppSettingsLoadResult::ReadFailure);

  remove(path);
#if defined(_WIN32)
  _rmdir(dir);
#else
  rmdir(dir);
#endif
  ArenaRelease(&a);
}
