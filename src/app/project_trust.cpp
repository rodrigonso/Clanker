#include "app/project_trust.h"

#include "app/config_paths.h"

#include "base/config_file.h"
#include "base/process_file_lock.h"
#include "base/sysinfo.h"
#include "base/thread.h"
#include "json/json.h"
#include "tools/fs.h"

namespace {

u32 RootLength(StrView path) {
  if (!path.Length) return 0;
#if defined(_WIN32)
  if (path.Length >= 3 && path.Data[1] == ':' &&
      Fs_IsSep(path.Data[2]))
    return 3;
  if (path.Length >= 2 && Fs_IsSep(path.Data[0]) &&
      Fs_IsSep(path.Data[1])) {
    u32 separators = 0;
    for (u32 i = 2; i < path.Length; i++) {
      if (!Fs_IsSep(path.Data[i])) continue;
      separators++;
      while (i + 1 < path.Length && Fs_IsSep(path.Data[i + 1])) i++;
      if (separators == 2) return i + 1;
    }
    return path.Length;
  }
#endif
  return Fs_IsSep(path.Data[0]) ? 1u : 0u;
}

bool PathIsAbsolute(StrView path) {
#if defined(_WIN32)
  return (path.Length >= 3 && path.Data[1] == ':' &&
          Fs_IsSep(path.Data[2])) ||
         (path.Length >= 2 && Fs_IsSep(path.Data[0]) &&
          Fs_IsSep(path.Data[1]));
#else
  return path.Length && Fs_IsSep(path.Data[0]);
#endif
}

bool PathIsAncestor(StrView ancestor, StrView path) {
  if (ancestor.Length > path.Length) return false;
  for (u32 i = 0; i < ancestor.Length; i++) {
    if (Fs_PathFold(ancestor.Data[i]) != Fs_PathFold(path.Data[i])) return false;
  }
  if (ancestor.Length == path.Length) return true;
  if (ancestor.Length == RootLength(ancestor)) return true;
  return Fs_IsSep(path.Data[ancestor.Length]);
}

i32 ComparePath(StrView a, StrView b) {
  u32 common = a.Length < b.Length ? a.Length : b.Length;
  for (u32 i = 0; i < common; i++) {
    char ac = Fs_PathFold(a.Data[i]);
    char bc = Fs_PathFold(b.Data[i]);
    if (ac < bc) return -1;
    if (ac > bc) return 1;
  }
  if (a.Length < b.Length) return -1;
  if (a.Length > b.Length) return 1;
  return 0;
}

i32 FindExact(const ProjectTrustStore* store, StrView path) {
  if (!store) return -1;
  for (u32 i = 0; i < store->Entries.Count; i++) {
    if (Fs_PathEquals(store->Entries.Data[i].Path, path)) return (i32)i;
  }
  return -1;
}

bool WriteStore(Arena* scratch, StrView storePath,
                ProjectTrustStore* store) {
  for (u32 i = 1; i < store->Entries.Count; i++) {
    ProjectTrustEntry value = store->Entries.Data[i];
    u32 at = i;
    while (at && ComparePath(value.Path, store->Entries.Data[at - 1].Path) < 0) {
      store->Entries.Data[at] = store->Entries.Data[at - 1];
      at--;
    }
    store->Entries.Data[at] = value;
  }

  StrBuf json = StrBuf_Make(scratch, 256);
  StrBuf_AppendCStr(&json, "{\n");
  for (u32 i = 0; i < store->Entries.Count; i++) {
    StrBuf_AppendCStr(&json, "  ");
    Json_WriteString(&json, store->Entries.Data[i].Path);
    StrBuf_AppendCStr(&json, store->Entries.Data[i].Trusted ? ": true" : ": false");
    if (i + 1 < store->Entries.Count) StrBuf_AppendChar(&json, ',');
    StrBuf_AppendChar(&json, '\n');
  }
  StrBuf_AppendCStr(&json, "}\n");
  return ConfigFile_WriteAtomic(scratch, storePath, StrBuf_View(&json));
}

}  // namespace

StrView DefaultProjectTrust_Name(DefaultProjectTrust value) {
  switch (value) {
    case DefaultProjectTrust::Ask: return SVL("ask");
    case DefaultProjectTrust::Always: return SVL("always");
    case DefaultProjectTrust::Never: return SVL("never");
  }
  return {};
}

bool DefaultProjectTrust_Parse(StrView value, DefaultProjectTrust* out) {
  if (StrView_Eq(value, SVL("ask"))) {
    *out = DefaultProjectTrust::Ask;
    return true;
  }
  if (StrView_Eq(value, SVL("always"))) {
    *out = DefaultProjectTrust::Always;
    return true;
  }
  if (StrView_Eq(value, SVL("never"))) {
    *out = DefaultProjectTrust::Never;
    return true;
  }
  return false;
}

StrView ProjectTrust_DefaultPath(Arena* arena) {
  return ClankDataPath(arena, "trust.json");
}

StrView ProjectTrust_CanonicalPath(Arena* arena, StrView path) {
  StrView real = Fs_RealPath(arena, path);
  StrBuf normalized = StrBuf_Make(arena, real.Length + 1);
  for (u32 i = 0; i < real.Length; i++)
    StrBuf_AppendChar(&normalized, Fs_IsSep(real.Data[i]) ? '/' : real.Data[i]);
  u32 root = RootLength(StrBuf_View(&normalized));
  while (normalized.Length > root &&
         normalized.Data[normalized.Length - 1] == '/')
    normalized.Data[--normalized.Length] = '\0';
  return StrBuf_View(&normalized);
}

StrView ProjectTrust_ParentPath(Arena* arena, StrView path) {
  StrView canonical = ProjectTrust_CanonicalPath(arena, path);
  StrView parent = Fs_Parent(canonical);
  if (parent.Length) return StrView_Dup(arena, parent);
  u32 rootLength = RootLength(canonical);
  if (rootLength && rootLength < canonical.Length)
    return StrView_Dup(arena, StrView{canonical.Data, rootLength});
  return {};
}

bool ProjectTrust_HasGatedResources(Arena* arena, StrView workingDirectory) {
  StrView config = Fs_Join(arena, workingDirectory, SVL(".clank"));
  const StrView names[] = {SVL("SYSTEM.md"), SVL("APPEND_SYSTEM.md")};
  for (u32 i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    FsStat stat = Fs_Stat(Fs_Join(arena, config, names[i]));
    if (stat.Exists && !stat.IsDir) return true;
  }
  return false;
}

ProjectTrustStoreLoadResult ProjectTrustStore_Load(Arena* arena, StrView path,
                                                   ProjectTrustStore* out) {
  out->Entries = Array_Make<ProjectTrustEntry>(arena, 8);
  StrView content = {};
  ConfigFileReadResult read = ConfigFile_ReadAll(arena, path, &content);
  if (read == ConfigFileReadResult::Missing)
    return ProjectTrustStoreLoadResult::Missing;
  if (read == ConfigFileReadResult::Failure)
    return ProjectTrustStoreLoadResult::ReadFailure;

  JsonValue* root = Json_Parse(arena, content);
  if (!Json_IsObject(root)) return ProjectTrustStoreLoadResult::Malformed;
  for (u32 i = 0; i < root->MemberCount; i++) {
    const JsonMember& member = root->Members[i];
    if (!member.Key.Length || !member.Value ||
        member.Value->Kind != JsonKind::Bool ||
        !PathIsAbsolute(member.Key))
      return ProjectTrustStoreLoadResult::Malformed;
    StrView canonical = ProjectTrust_CanonicalPath(arena, member.Key);
    if (!canonical.Length || FindExact(out, canonical) >= 0)
      return ProjectTrustStoreLoadResult::Malformed;
    Array_Push(&out->Entries,
               ProjectTrustEntry{canonical, member.Value->Bool});
  }
  return ProjectTrustStoreLoadResult::Success;
}

ProjectTrustMatch ProjectTrustStore_Find(const ProjectTrustStore* store,
                                         StrView path) {
  ProjectTrustMatch match = {};
  if (!store) return match;
  for (u32 i = 0; i < store->Entries.Count; i++) {
    const ProjectTrustEntry& entry = store->Entries.Data[i];
    if (!PathIsAncestor(entry.Path, path)) continue;
    if (!match.Found || entry.Path.Length > match.Path.Length) {
      match.Found = true;
      match.Trusted = entry.Trusted;
      match.Path = entry.Path;
    }
  }
  return match;
}

ProjectTrustResolution ProjectTrust_Resolve(bool hasGatedResources,
                                            ProjectTrustOverride overrideValue,
                                            const ProjectTrustStore* store,
                                            StrView workingDirectory,
                                            DefaultProjectTrust defaultValue,
                                            bool interactive) {
  ProjectTrustResolution result = {};
  if (!hasGatedResources) {
    result.Trusted = true;
    result.Source = ProjectTrustResolutionSource::NoResources;
    return result;
  }
  if (overrideValue != ProjectTrustOverride::Unspecified) {
    result.Trusted = overrideValue == ProjectTrustOverride::Trusted;
    result.Source = ProjectTrustResolutionSource::CommandLine;
    return result;
  }
  result.Match = ProjectTrustStore_Find(store, workingDirectory);
  if (result.Match.Found) {
    result.Trusted = result.Match.Trusted;
    result.Source = ProjectTrustResolutionSource::SavedDecision;
    return result;
  }
  if (defaultValue == DefaultProjectTrust::Always ||
      defaultValue == DefaultProjectTrust::Never) {
    result.Trusted = defaultValue == DefaultProjectTrust::Always;
    result.Source = ProjectTrustResolutionSource::GlobalDefault;
    return result;
  }
  if (interactive) {
    result.NeedsPrompt = true;
    result.Source = ProjectTrustResolutionSource::InteractivePrompt;
    return result;
  }
  result.Trusted = false;
  result.Source = ProjectTrustResolutionSource::NonInteractiveDefault;
  return result;
}

ProjectTrustReloadResolution ProjectTrust_ResolveReload(
    bool hasGatedResourcesNow, bool trustCoversProjectResources,
    ProjectTrustOverride overrideValue, bool currentTrusted) {
  if (!hasGatedResourcesNow)
    return ProjectTrustReloadResolution{true, currentTrusted};
  if (overrideValue != ProjectTrustOverride::Unspecified)
    return ProjectTrustReloadResolution{
        true, overrideValue == ProjectTrustOverride::Trusted};
  if (trustCoversProjectResources)
    return ProjectTrustReloadResolution{true, currentTrusted};
  return ProjectTrustReloadResolution{false, false};
}

bool ProjectTrustStore_Update(Arena* scratch, StrView storePath,
                              StrView decisionPath, bool trusted,
                              StrView removePath, StrBuf* error) {
  if (!scratch || !storePath.Length || !decisionPath.Length || !error) return false;
  StrView parent = Fs_Parent(storePath);
  if (parent.Length && !Fs_MkdirP(scratch, parent)) {
    StrBuf_AppendCStr(error, "could not create trust store directory");
    return false;
  }

  StrBuf lockPath = StrBuf_Make(scratch, storePath.Length + 8);
  StrBuf_Append(&lockPath, storePath);
  StrBuf_AppendCStr(&lockPath, ".lock");
  ProcessFileLock lock = {};
  ProcessFileLockStatus lockStatus = ProcessFileLockStatus::Busy;
  for (u32 attempt = 0; attempt < 10; attempt++) {
    lockStatus =
        ProcessFileLock_TryAcquire(scratch, StrBuf_View(&lockPath), &lock);
    if (lockStatus != ProcessFileLockStatus::Busy) break;
    Sleep_Ms(20);
  }
  if (lockStatus != ProcessFileLockStatus::Acquired) {
    StrBuf_AppendCStr(error, lockStatus == ProcessFileLockStatus::Busy
                                ? "trust store is busy"
                                : "could not lock trust store");
    return false;
  }

  ProjectTrustStore store = {};
  ProjectTrustStoreLoadResult loaded =
      ProjectTrustStore_Load(scratch, storePath, &store);
  if (loaded == ProjectTrustStoreLoadResult::Malformed ||
      loaded == ProjectTrustStoreLoadResult::ReadFailure) {
    StrBuf_AppendCStr(error, loaded == ProjectTrustStoreLoadResult::Malformed
                                ? "trust store is malformed"
                                : "trust store could not be read");
    ProcessFileLock_Release(&lock, StrBuf_View(&lockPath));
    return false;
  }

  StrView canonicalDecision =
      ProjectTrust_CanonicalPath(scratch, decisionPath);
  StrView canonicalRemove =
      removePath.Length ? ProjectTrust_CanonicalPath(scratch, removePath)
                        : StrView{};
  if (canonicalRemove.Length) {
    i32 removeIndex = FindExact(&store, canonicalRemove);
    if (removeIndex >= 0) {
      for (u32 i = (u32)removeIndex + 1; i < store.Entries.Count; i++)
        store.Entries.Data[i - 1] = store.Entries.Data[i];
      store.Entries.Count--;
    }
  }
  i32 existing = FindExact(&store, canonicalDecision);
  if (existing >= 0)
    store.Entries.Data[existing].Trusted = trusted;
  else
    Array_Push(&store.Entries,
               ProjectTrustEntry{canonicalDecision, trusted});

  bool saved = WriteStore(scratch, storePath, &store);
  ProcessFileLock_Release(&lock, StrBuf_View(&lockPath));
  if (!saved) StrBuf_AppendCStr(error, "trust store could not be saved");
  return saved;
}
