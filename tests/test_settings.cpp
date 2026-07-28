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
  CHECK(loaded.DefaultTrust == DefaultProjectTrust::Ask);

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
  CHECK(AppSettings_Save(&a, SV(path), &saved));
  CHECK(AppSettings_Load(&a, SV(path), &loaded) == AppSettingsLoadResult::Success);
  CHECK_ALL(StrView_Eq(loaded.Provider, saved.Provider),
            StrView_Eq(loaded.Model, saved.Model),
            loaded.DefaultTrust == DefaultProjectTrust::Always);

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
