#include <stdio.h>
#if defined(_WIN32)
#include <direct.h>
#define RemoveDir _rmdir
#else
#include <unistd.h>
#define RemoveDir rmdir
#endif

#include "app/update.h"
#include "base/arena.h"
#include "base/config_file.h"
#include "check.h"

static void TestVersions() {
  u32 v[3];
  CHECK(Version_Parse(SVL("0.1.0"), v));
  CHECK_ALL(v[0] == 0, v[1] == 1, v[2] == 0);
  CHECK(Version_Parse(SVL("v12.3.45"), v));
  CHECK_ALL(v[0] == 12, v[1] == 3, v[2] == 45);
  const char* bad[] = {"", "1.2", "1.2.3.4", "1.2.x", "1..2", "v", "1.2.3-alpha", " 1.2.3",
                       "1234567890.0.0"};
  for (const char* text : bad) {
    CHECK(!Version_Parse(SV(text), v));
  }

  CHECK(Version_Compare(SVL("0.2.0"), SVL("0.1.9")) > 0);
  CHECK(Version_Compare(SVL("0.10.0"), SVL("0.9.0")) > 0);
  CHECK(Version_Compare(SVL("v1.0.0"), SVL("1.0.0")) == 0);
  CHECK(Version_Compare(SVL("1.0.0"), SVL("1.0.1")) < 0);
  CHECK(Version_Compare(SVL("garbage"), SVL("1.0.0")) == 0);
}

static void TestReleases() {
  Arena a = ArenaCreate();
  CHECK(StrView_EqCStr(
      Releases_ParseLatest(&a, SVL("[{\"tag_name\":\"v0.2.0\",\"prerelease\":true},"
                                   "{\"tag_name\":\"v0.1.0\"}]")),
      "0.2.0"));
  CHECK(Releases_ParseLatest(&a, SVL("[]")).Length == 0);
  CHECK(Releases_ParseLatest(&a, SVL("{\"message\":\"rate limited\"}")).Length == 0);
  CHECK(Releases_ParseLatest(&a, SVL("[{\"tag_name\":\"nightly\"}]")).Length == 0);
  ArenaRelease(&a);
}

static void TestUpdateCache() {
  Arena a = ArenaCreate();
  const u64 day = 24ull * 60 * 60 * 1000;

  UpdateCache written = {1700000000000ull, SVL("0.3.1")};
  UpdateCache read = {};
  CHECK(UpdateCache_Parse(&a, UpdateCache_Format(&a, &written), &read));
  CHECK_ALL(read.CheckedAtMs == written.CheckedAtMs, StrView_EqCStr(read.Latest, "0.3.1"));

  // A corrupt version is dropped instead of being shown in a notice.
  CHECK(UpdateCache_Parse(&a, SVL("{\"checked_at\":5,\"latest\":\"oops\"}"), &read));
  CHECK_ALL(read.CheckedAtMs == 5, read.Latest.Length == 0);
  CHECK(!UpdateCache_Parse(&a, SVL("not json"), &read));

  UpdateCache fresh = {10 * day, {}};
  CHECK(!UpdateCache_IsStale(&fresh, 10 * day + day - 1));
  CHECK(UpdateCache_IsStale(&fresh, 11 * day));
  CHECK(UpdateCache_IsStale(&fresh, 9 * day));  // clock moved backwards
  UpdateCache never = {};
  CHECK(UpdateCache_IsStale(&never, 10 * day));

  UpdateCache newer = {1, SVL("0.2.0")};
  CHECK(StrView_EqCStr(UpdateNotice(&a, &newer, SVL("0.1.0")),
                       "Clanker 0.2.0 is available (you have 0.1.0). "
                       "Run `clanker upgrade` to install it."));
  CHECK(UpdateNotice(&a, &newer, SVL("0.2.0")).Length == 0);
  CHECK(UpdateNotice(&a, &newer, SVL("0.3.0")).Length == 0);
  CHECK(UpdateNotice(&a, &never, SVL("0.1.0")).Length == 0);
  ArenaRelease(&a);
}

static void TestInstallPrefix() {
  Arena a = ArenaCreate();
  const char* marker = "clanker_update_test/share/clanker/DISTRIBUTION.txt";
  remove(marker);
  CHECK(Install_PrefixFromExe(&a, SVL("clanker_update_test/bin/clanker.exe")).Length == 0);

  // EnsureParentDir creates one level, so build the tree top-down.
  CHECK(ConfigFile_WriteAtomic(&a, SVL("clanker_update_test/x"), SVL("")));
  CHECK(ConfigFile_WriteAtomic(&a, SVL("clanker_update_test/share/x"), SVL("")));
  CHECK(ConfigFile_WriteAtomic(&a, SVL("clanker_update_test/share/clanker/x"), SVL("")));
  CHECK(ConfigFile_WriteAtomic(&a, SV(marker), SVL("Clanker")));
  CHECK(StrView_EqCStr(Install_PrefixFromExe(&a, SVL("clanker_update_test/bin/clanker.exe")),
                       "clanker_update_test"));
  CHECK(StrView_EqCStr(Install_PrefixFromExe(&a, SVL("clanker_update_test\\BIN\\clanker.exe")),
                       "clanker_update_test"));
  // Only <prefix>/bin/<exe> counts; a build directory next to the marker does not.
  CHECK(Install_PrefixFromExe(&a, SVL("clanker_update_test/build/clanker.exe")).Length == 0);
  CHECK(Install_PrefixFromExe(&a, SVL("clanker.exe")).Length == 0);

  remove(marker);
  remove("clanker_update_test/share/clanker/x");
  remove("clanker_update_test/share/x");
  remove("clanker_update_test/x");
  RemoveDir("clanker_update_test/share/clanker");
  RemoveDir("clanker_update_test/share");
  RemoveDir("clanker_update_test");
  ArenaRelease(&a);
}

void TestUpdate() {
  TestVersions();
  TestReleases();
  TestUpdateCache();
  TestInstallPrefix();
}
