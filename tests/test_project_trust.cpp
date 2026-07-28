#include <stdio.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "app/project_trust.h"
#include "base/config_file.h"
#include "check.h"
#include "tools/fs.h"

namespace {

void RemoveDir(const char* path) {
#if defined(_WIN32)
  _rmdir(path);
#else
  rmdir(path);
#endif
}

}  // namespace

void TestProjectTrust() {
  Arena scratch = ArenaCreate((usize)64 << 20);
  StrView root = SVL("clank_project_trust_test");
  StrView parent = Fs_Join(&scratch, root, SVL("parent"));
  StrView child = Fs_Join(&scratch, parent, SVL("child"));
  StrView config = Fs_Join(&scratch, child, SVL(".clank"));
  StrView system = Fs_Join(&scratch, config, SVL("SYSTEM.md"));
  StrView storePath = Fs_Join(&scratch, root, SVL("config/trust.json"));
  CHECK(Fs_MkdirP(&scratch, child));
  CHECK(!ProjectTrust_HasGatedResources(&scratch, child));
  CHECK(ConfigFile_WriteAtomic(&scratch, system, SVL("project system")));
  CHECK(ProjectTrust_HasGatedResources(&scratch, child));

  ProjectTrustStore store = {};
  CHECK(ProjectTrustStore_Load(&scratch, storePath, &store) ==
        ProjectTrustStoreLoadResult::Missing);

  StrBuf error = StrBuf_Make(&scratch, 128);
  CHECK(ProjectTrustStore_Update(&scratch, storePath, parent, true, {}, &error));
  CHECK(ProjectTrustStore_Load(&scratch, storePath, &store) ==
        ProjectTrustStoreLoadResult::Success);
  StrView canonicalChild = ProjectTrust_CanonicalPath(&scratch, child);
  ProjectTrustMatch match = ProjectTrustStore_Find(&store, canonicalChild);
  CHECK_ALL(match.Found, match.Trusted);

  StrBuf_Clear(&error);
  CHECK(ProjectTrustStore_Update(&scratch, storePath, child, false, {}, &error));
  CHECK(ProjectTrustStore_Load(&scratch, storePath, &store) ==
        ProjectTrustStoreLoadResult::Success);
  match = ProjectTrustStore_Find(&store, canonicalChild);
  CHECK_ALL(match.Found, !match.Trusted);

  StrBuf_Clear(&error);
  CHECK(ProjectTrustStore_Update(&scratch, storePath, parent, true, child, &error));
  CHECK(ProjectTrustStore_Load(&scratch, storePath, &store) ==
        ProjectTrustStoreLoadResult::Success);
  match = ProjectTrustStore_Find(&store, canonicalChild);
  CHECK_ALL(match.Found, match.Trusted, store.Entries.Count == 1);

  ProjectTrustResolution resolved = ProjectTrust_Resolve(
      true, ProjectTrustOverride::Unspecified, &store, canonicalChild,
      DefaultProjectTrust::Ask, true);
  CHECK_ALL(resolved.Trusted, !resolved.NeedsPrompt,
            resolved.Source == ProjectTrustResolutionSource::SavedDecision);
  resolved = ProjectTrust_Resolve(
      true, ProjectTrustOverride::Unspecified, nullptr, canonicalChild,
      DefaultProjectTrust::Ask, false);
  CHECK_ALL(!resolved.Trusted, !resolved.NeedsPrompt,
            resolved.Source ==
                ProjectTrustResolutionSource::NonInteractiveDefault);
  resolved = ProjectTrust_Resolve(
      true, ProjectTrustOverride::Unspecified, nullptr, canonicalChild,
      DefaultProjectTrust::Ask, true);
  CHECK(resolved.NeedsPrompt);
  resolved = ProjectTrust_Resolve(
      false, ProjectTrustOverride::Untrusted, nullptr, canonicalChild,
      DefaultProjectTrust::Never, false);
  CHECK_ALL(resolved.Trusted,
            resolved.Source == ProjectTrustResolutionSource::NoResources);

  ProjectTrustReloadResolution reload = ProjectTrust_ResolveReload(
      true, false, ProjectTrustOverride::Unspecified, true);
  CHECK(!reload.CanReload);
  reload = ProjectTrust_ResolveReload(
      true, false, ProjectTrustOverride::Untrusted, true);
  CHECK_ALL(reload.CanReload, !reload.Trusted);
  reload = ProjectTrust_ResolveReload(
      true, true, ProjectTrustOverride::Unspecified, false);
  CHECK_ALL(reload.CanReload, !reload.Trusted);
  reload = ProjectTrust_ResolveReload(
      false, false, ProjectTrustOverride::Unspecified, true);
  CHECK_ALL(reload.CanReload, reload.Trusted);

  CHECK(ConfigFile_WriteAtomic(&scratch, storePath, SVL("{\"relative\":true}")));
  CHECK(ProjectTrustStore_Load(&scratch, storePath, &store) ==
        ProjectTrustStoreLoadResult::Malformed);

  remove("clank_project_trust_test/config/trust.json");
  remove("clank_project_trust_test/config/trust.json.lock");
  remove("clank_project_trust_test/parent/child/.clank/SYSTEM.md");
  RemoveDir("clank_project_trust_test/parent/child/.clank");
  RemoveDir("clank_project_trust_test/parent/child");
  RemoveDir("clank_project_trust_test/parent");
  RemoveDir("clank_project_trust_test/config");
  RemoveDir("clank_project_trust_test");
  ArenaRelease(&scratch);
}
