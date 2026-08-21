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
  const char* dir = "clanker_settings_test";
  const char* path = "clanker_settings_test/settings.json";
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
            loaded.Profile.ContextWindow == 0, loaded.Rounded,
            loaded.PermissionMode == AgentPermissionMode::Auto,
            StrView_Eq(loaded.PermissionReviewerModel,
                       AGENT_PERMISSION_DEFAULT_REVIEWER_MODEL),
            !loaded.YoloAcknowledged);
  // An absent compaction block leaves the built-in defaults in place.
  CHECK_ALL(loaded.Compaction.Enabled,
            loaded.Compaction.ReserveTokens ==
                AGENT_COMPACTION_DEFAULT_RESERVE_TOKENS,
            loaded.Compaction.KeepRecentTokens ==
                AGENT_COMPACTION_DEFAULT_KEEP_RECENT_TOKENS);

  CHECK_ALL(loaded.Retry.Enabled,
            loaded.Retry.MaxRetries == AGENT_RETRY_DEFAULT_MAX_RETRIES,
            loaded.Retry.BaseDelayMs == AGENT_RETRY_DEFAULT_BASE_DELAY_MS,
            loaded.Retry.MaxDelayMs == AGENT_RETRY_DEFAULT_MAX_DELAY_MS);

  CHECK_ALL(loaded.TurnLimit.Enabled,
            loaded.TurnLimit.MaxTurns == AGENT_TURN_LIMIT_DEFAULT_MAX_TURNS);

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
      SVL("{\"provider\":\"anthropic\",\"model\":\"legacy\","
          "\"retry\":{\"maxRetries\":0}}")));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) ==
        AppSettingsLoadResult::Success);
  CHECK_ALL(loaded.Retry.Enabled, loaded.Retry.MaxRetries == 0,
            loaded.Retry.BaseDelayMs == AGENT_RETRY_DEFAULT_BASE_DELAY_MS,
            loaded.Retry.MaxDelayMs == AGENT_RETRY_DEFAULT_MAX_DELAY_MS);

  CHECK(ConfigFile_WriteAtomic(
      &a, SV(path),
      SVL("{\"provider\":\"anthropic\",\"model\":\"legacy\","
          "\"retry\":{\"enabled\":false,\"baseDelayMs\":500,"
          "\"maxDelayMs\":1500}}")));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) ==
        AppSettingsLoadResult::Success);
  CHECK_ALL(!loaded.Retry.Enabled,
            loaded.Retry.MaxRetries == AGENT_RETRY_DEFAULT_MAX_RETRIES,
            loaded.Retry.BaseDelayMs == 500, loaded.Retry.MaxDelayMs == 1500);

  CHECK(ConfigFile_WriteAtomic(
      &a, SV(path),
      SVL("{\"provider\":\"anthropic\",\"model\":\"bad\",\"retry\":true}")));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) ==
        AppSettingsLoadResult::Malformed);

  CHECK(ConfigFile_WriteAtomic(
      &a, SV(path),
      SVL("{\"provider\":\"anthropic\",\"model\":\"bad\","
          "\"retry\":{\"maxRetries\":-1}}")));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) ==
        AppSettingsLoadResult::Malformed);

  CHECK(ConfigFile_WriteAtomic(
      &a, SV(path),
      SVL("{\"provider\":\"anthropic\",\"model\":\"bad\","
          "\"retry\":{\"maxRetries\":1.5}}")));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) ==
        AppSettingsLoadResult::Malformed);

  CHECK(ConfigFile_WriteAtomic(
      &a, SV(path),
      SVL("{\"provider\":\"anthropic\",\"model\":\"bad\","
          "\"retry\":{\"enabled\":\"yes\"}}")));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) ==
        AppSettingsLoadResult::Malformed);

  CHECK(ConfigFile_WriteAtomic(
      &a, SV(path),
      SVL("{\"provider\":\"anthropic\",\"model\":\"bad\","
          "\"retry\":{\"baseDelayMs\":0}}")));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) ==
        AppSettingsLoadResult::Malformed);

  // maxTurns 0 means "no cap" and must survive the loader rather than being
  // rejected the way a zero token count is.
  CHECK(ConfigFile_WriteAtomic(
      &a, SV(path),
      SVL("{\"provider\":\"anthropic\",\"model\":\"legacy\","
          "\"turnLimit\":{\"maxTurns\":0}}")));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) ==
        AppSettingsLoadResult::Success);
  CHECK_ALL(loaded.TurnLimit.Enabled, loaded.TurnLimit.MaxTurns == 0,
            !AgentTurnLimitPolicy_IsValid(&loaded.TurnLimit));

  CHECK(ConfigFile_WriteAtomic(
      &a, SV(path),
      SVL("{\"provider\":\"anthropic\",\"model\":\"legacy\","
          "\"turnLimit\":{\"enabled\":false}}")));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) ==
        AppSettingsLoadResult::Success);
  CHECK_ALL(!loaded.TurnLimit.Enabled,
            loaded.TurnLimit.MaxTurns == AGENT_TURN_LIMIT_DEFAULT_MAX_TURNS);

  CHECK(ConfigFile_WriteAtomic(
      &a, SV(path),
      SVL("{\"provider\":\"anthropic\",\"model\":\"bad\",\"turnLimit\":7}")));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) ==
        AppSettingsLoadResult::Malformed);

  CHECK(ConfigFile_WriteAtomic(
      &a, SV(path),
      SVL("{\"provider\":\"anthropic\",\"model\":\"bad\","
          "\"turnLimit\":{\"maxTurns\":-1}}")));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) ==
        AppSettingsLoadResult::Malformed);

  CHECK(ConfigFile_WriteAtomic(
      &a, SV(path),
      SVL("{\"provider\":\"anthropic\",\"model\":\"bad\","
          "\"turnLimit\":{\"maxTurns\":2.5}}")));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) ==
        AppSettingsLoadResult::Malformed);

  CHECK(ConfigFile_WriteAtomic(
      &a, SV(path),
      SVL("{\"provider\":\"anthropic\",\"model\":\"bad\","
          "\"turnLimit\":{\"enabled\":1}}")));
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

  CHECK(ConfigFile_WriteAtomic(
      &a, SV(path),
      SVL("{\"provider\":\"anthropic\",\"model\":\"bad\","
          "\"rounded\":\"yes\"}")));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) ==
        AppSettingsLoadResult::Malformed);

  CHECK(ConfigFile_WriteAtomic(
      &a, SV(path),
      SVL("{\"provider\":\"anthropic\",\"model\":\"bad\","
          "\"permissionMode\":\"always\"}")));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) ==
        AppSettingsLoadResult::Malformed);
  CHECK(ConfigFile_WriteAtomic(
      &a, SV(path),
      SVL("{\"provider\":\"anthropic\",\"model\":\"bad\","
          "\"permissionMode\":true}")));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) ==
        AppSettingsLoadResult::Malformed);
  CHECK(ConfigFile_WriteAtomic(
      &a, SV(path),
      SVL("{\"provider\":\"anthropic\",\"model\":\"bad\","
          "\"permissionReviewerModel\":\"\"}")));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) ==
        AppSettingsLoadResult::Malformed);
  CHECK(ConfigFile_WriteAtomic(
      &a, SV(path),
      SVL("{\"provider\":\"anthropic\",\"model\":\"bad\","
          "\"yoloAcknowledged\":\"yes\"}")));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) ==
        AppSettingsLoadResult::Malformed);

  CHECK(ConfigFile_WriteAtomic(
      &a, SV(path),
      SVL("{\"provider\":\"anthropic\",\"model\":\"legacy\","
          "\"rounded\":false}")));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) ==
        AppSettingsLoadResult::Success);
  CHECK(!loaded.Rounded);

  AppSettings saved = {};
  saved.Provider = SVL("anthropic");
  saved.Model = SVL("claude-\"test\"");
  saved.DefaultTrust = DefaultProjectTrust::Always;
  saved.Profile.Effort = ModelEffort::High;
  saved.Profile.ContextWindow = 1000000;
  saved.Compaction.Enabled = true;
  saved.Compaction.ReserveTokens = 4096;
  saved.Compaction.KeepRecentTokens = 8192;
  saved.Retry.Enabled = true;
  saved.Retry.MaxRetries = 5;
  saved.Retry.BaseDelayMs = 250;
  saved.Retry.MaxDelayMs = 3000;
  saved.TurnLimit.Enabled = true;
  saved.TurnLimit.MaxTurns = 42;
  saved.PermissionMode = AgentPermissionMode::Yolo;
  saved.PermissionReviewerModel = SVL("z-ai/glm-5.2");
  saved.YoloAcknowledged = true;
  CHECK(AppSettings_Save(&a, SV(path), &saved));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) == AppSettingsLoadResult::Success);
  CHECK_ALL(StrView_Eq(loaded.Provider, saved.Provider),
            StrView_Eq(loaded.Model, saved.Model),
            loaded.DefaultTrust == DefaultProjectTrust::Always,
  loaded.Profile.Effort == ModelEffort::High,
  loaded.Profile.ContextWindow == 1000000);
  CHECK_ALL(loaded.Compaction.Enabled, loaded.Compaction.ReserveTokens == 4096,
            loaded.Compaction.KeepRecentTokens == 8192, loaded.Rounded);
  CHECK_ALL(loaded.Retry.Enabled, loaded.Retry.MaxRetries == 5,
            loaded.Retry.BaseDelayMs == 250, loaded.Retry.MaxDelayMs == 3000);
  CHECK_ALL(loaded.TurnLimit.Enabled, loaded.TurnLimit.MaxTurns == 42);
  CHECK_ALL(loaded.PermissionMode == AgentPermissionMode::Yolo,
            StrView_EqCStr(loaded.PermissionReviewerModel, "z-ai/glm-5.2"),
            loaded.YoloAcknowledged);

  saved.Rounded = false;
  CHECK(AppSettings_Save(&a, SV(path), &saved));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) == AppSettingsLoadResult::Success);
  CHECK(!loaded.Rounded);
  saved.Rounded = true;

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
  zeroed.Retry = AgentRetryPolicy{};
  zeroed.TurnLimit = AgentTurnLimitPolicy{};
  CHECK(AppSettings_Save(&a, SV(path), &zeroed));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) == AppSettingsLoadResult::Success);
  CHECK_ALL(!loaded.Compaction.Enabled,
            loaded.Compaction.ReserveTokens ==
                AGENT_COMPACTION_DEFAULT_RESERVE_TOKENS,
            loaded.Compaction.KeepRecentTokens ==
                AGENT_COMPACTION_DEFAULT_KEEP_RECENT_TOKENS);
  CHECK_ALL(!loaded.Retry.Enabled, loaded.Retry.MaxRetries == 0,
            loaded.Retry.BaseDelayMs == AGENT_RETRY_DEFAULT_BASE_DELAY_MS,
            loaded.Retry.MaxDelayMs == AGENT_RETRY_DEFAULT_MAX_DELAY_MS);
  // Unlike the delays, maxTurns has no floor to restore: 0 is a legal value
  // that means unlimited, so it must come back as written.
  CHECK_ALL(!loaded.TurnLimit.Enabled, loaded.TurnLimit.MaxTurns == 0);

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
