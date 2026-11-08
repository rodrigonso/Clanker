#include <stdio.h>
#include <math.h>

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
            !loaded.PermissionReviewerInvalid,
            loaded.PermissionReviewerThreshold == 0.95,
            !loaded.PermissionReviewerThresholdInvalid,
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

  const char* invalidThresholds[] = {
      "\"0.9\"", "null", "true", "[]", "{}", "-0.01", "1.01", "1e999", "-1e999"};
  for (const char* threshold : invalidThresholds) {
    StrBuf content = StrBuf_Make(&a, 160);
    StrBuf_AppendCStr(&content,
        "{\"provider\":\"openrouter\",\"model\":\"chat\",\"permissionReviewerThreshold\":");
    StrBuf_AppendCStr(&content, threshold);
    StrBuf_AppendChar(&content, '}');
    CHECK(ConfigFile_WriteAtomic(&a, SV(path), StrBuf_View(&content)));
    CHECK(AppSettings_Load(&a, SV(path), &loaded) == AppSettingsLoadResult::Malformed);
    // A non-finite number is rejected by the JSON parser itself, before the
    // threshold field can be singled out.
    bool parsable = !StrView_Contains(SV(threshold), "e999");
    CHECK(loaded.PermissionReviewerThresholdInvalid == parsable);
  }
  AppSettings thresholdSettings = {};
  thresholdSettings.Provider = SVL("openrouter");
  thresholdSettings.Model = SVL("chat");
  const f64 thresholds[] = {0, 1, 0.95, 0.9000000000000001};
  for (f64 threshold : thresholds) {
    thresholdSettings.PermissionReviewerThreshold = threshold;
    CHECK(AppSettings_Save(&a, SV(path), &thresholdSettings));
    CHECK(AppSettings_Load(&a, SV(path), &loaded) == AppSettingsLoadResult::Success);
    CHECK_ALL(loaded.PermissionReviewerThreshold == threshold,
              !loaded.PermissionReviewerThresholdInvalid);
  }
  const f64 invalidValues[] = {-0.01, 1.01, (f64)NAN, (f64)INFINITY, -(f64)INFINITY};
  for (f64 threshold : invalidValues) {
    thresholdSettings.PermissionReviewerThreshold = threshold;
    CHECK(!AppSettings_Save(&a, SV(path), &thresholdSettings));
    CHECK(AppSettings_Load(&a, SV(path), &loaded) == AppSettingsLoadResult::Success);
    CHECK(loaded.PermissionReviewerThreshold == 0.9000000000000001);
  }

  const char* invalidReviewers[] = {
      "\"\"", "null", "true", "42", "[]", "{}", "\"   \"", "\"jev-\"",
      "\"jev-1.13.0/invalid\"", "\" Jev-1.13.0\"", "\"z-ai/glm-5.2\"",
      "\"openrouter/auto\"", "\"reviewer/model\""};
  for (u32 i = 0; i < sizeof(invalidReviewers) / sizeof(invalidReviewers[0]);
       i++) {
    StrBuf content = StrBuf_Make(&a, 160);
    StrBuf_AppendCStr(
        &content,
        "{\"provider\":\"openrouter\",\"model\":\"chat\","
        "\"permissionReviewerModel\":");
    StrBuf_AppendCStr(&content, invalidReviewers[i]);
    StrBuf_AppendChar(&content, '}');
    CHECK(ConfigFile_WriteAtomic(&a, SV(path), StrBuf_View(&content)));
    CHECK(AppSettings_Load(&a, SV(path), &loaded) ==
          AppSettingsLoadResult::Malformed);
    CHECK(loaded.PermissionReviewerInvalid);
  }

  const char* jevReviewers[] = {"jev-1.13.0", "jev-1", "jev-latest"};
  for (u32 i = 0; i < sizeof(jevReviewers) / sizeof(jevReviewers[0]); i++) {
    AppSettings valid = {};
    valid.Provider = SVL("openrouter");
    valid.Model = SVL("chat");
    valid.PermissionReviewerModel = SV(jevReviewers[i]);
    CHECK(AppSettings_Save(&a, SV(path), &valid));
    CHECK(AppSettings_Load(&a, SV(path), &loaded) ==
          AppSettingsLoadResult::Success);
    CHECK_ALL(!loaded.PermissionReviewerInvalid,
              StrView_EqCStr(loaded.PermissionReviewerModel, jevReviewers[i]));
  }

  CHECK(ConfigFile_WriteAtomic(
      &a, SV(path),
      SVL("{\"provider\":\"openrouter\",\"model\":\"chat\","
          "\"permissionReviewerModel\":\"jev-1.12.0\","
          "\"permissionMode\":\"ask\",\"yoloAcknowledged\":true,"
          "\"defaultProjectTrust\":\"never\",\"effort\":\"high\","
          "\"contextWindow\":1000000,\"rounded\":false,"
          "\"memory\":false,\"dreamModel\":\"cheap-model\","
          "\"compaction\":{\"enabled\":false,\"reserveTokens\":4096,"
          "\"keepRecentTokens\":8192},\"retry\":{\"enabled\":false,"
          "\"maxRetries\":5,\"baseDelayMs\":250,\"maxDelayMs\":3000},"
          "\"turnLimit\":{\"enabled\":false,\"maxTurns\":42},"
          "\"skills\":[\"skills/a\"],\"promptTemplates\":[\"prompts/a\"]}")));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) ==
        AppSettingsLoadResult::Success);
  CHECK(AppSettings_Save(&a, SV(path), &loaded));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) ==
        AppSettingsLoadResult::Success);
  CHECK_ALL(StrView_EqCStr(loaded.PermissionReviewerModel, "jev-1.12.0"),
            StrView_EqCStr(loaded.Provider, "openrouter"),
            StrView_EqCStr(loaded.Model, "chat"),
            loaded.PermissionMode == AgentPermissionMode::Ask,
            loaded.YoloAcknowledged, !loaded.Rounded, !loaded.Memory,
            StrView_EqCStr(loaded.DreamModel, "cheap-model"),
            loaded.DefaultTrust == DefaultProjectTrust::Never,
            loaded.Profile.Effort == ModelEffort::High,
            loaded.Profile.ContextWindow == 1000000);
  CHECK_ALL(!loaded.Compaction.Enabled,
            loaded.Compaction.ReserveTokens == 4096,
            loaded.Compaction.KeepRecentTokens == 8192,
            !loaded.Retry.Enabled, loaded.Retry.MaxRetries == 5,
            loaded.Retry.BaseDelayMs == 250, loaded.Retry.MaxDelayMs == 3000,
            !loaded.TurnLimit.Enabled, loaded.TurnLimit.MaxTurns == 42,
            loaded.Skills.Count == 1, loaded.PromptTemplates.Count == 1);
  CHECK_ALL(StrView_EqCStr(loaded.Skills.Data[0], "skills/a"),
            StrView_EqCStr(loaded.PromptTemplates.Data[0], "prompts/a"));
  const char* unsupportedReviewers[] = {
      "jev-", "z-ai/glm-5.2", "openrouter/auto", "reviewer/model"};
  for (const char* reviewer : unsupportedReviewers) {
    loaded.PermissionReviewerModel = SV(reviewer);
    CHECK(!AppSettings_Save(&a, SV(path), &loaded));
    CHECK(AppSettings_Load(&a, SV(path), &loaded) ==
          AppSettingsLoadResult::Success);
    CHECK(StrView_EqCStr(loaded.PermissionReviewerModel, "jev-1.12.0"));
  }

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
  saved.PermissionReviewerModel = SVL("jev-1.13.0");
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
            StrView_EqCStr(loaded.PermissionReviewerModel, "jev-1.13.0"),
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
