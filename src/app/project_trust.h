#pragma once

#include "base/array.h"
#include "base/str.h"

enum class DefaultProjectTrust : u8 { Ask, Always, Never };

StrView DefaultProjectTrust_Name(DefaultProjectTrust value);
bool DefaultProjectTrust_Parse(StrView value, DefaultProjectTrust* out);

struct ProjectTrustEntry {
  StrView Path;
  bool Trusted;
};

struct ProjectTrustStore {
  Array<ProjectTrustEntry> Entries;
};

enum class ProjectTrustStoreLoadResult : u8 {
  Success,
  Missing,
  Malformed,
  ReadFailure,
};

struct ProjectTrustMatch {
  bool Found;
  bool Trusted;
  StrView Path;
};

enum class ProjectTrustOverride : u8 { Unspecified, Trusted, Untrusted };

enum class ProjectTrustResolutionSource : u8 {
  NoResources,
  CommandLine,
  SavedDecision,
  GlobalDefault,
  NonInteractiveDefault,
  InteractivePrompt,
};

struct ProjectTrustResolution {
  bool Trusted;
  bool NeedsPrompt;
  ProjectTrustResolutionSource Source;
  ProjectTrustMatch Match;
};

struct ProjectTrustReloadResolution {
  bool CanReload;
  bool Trusted;
};

StrView ProjectTrust_DefaultPath(Arena* arena);
StrView ProjectTrust_CanonicalPath(Arena* arena, StrView path);
StrView ProjectTrust_ParentPath(Arena* arena, StrView path);
bool ProjectTrust_HasGatedResources(Arena* arena, StrView workingDirectory);
ProjectTrustStoreLoadResult ProjectTrustStore_Load(Arena* arena, StrView path,
                                                   ProjectTrustStore* out);
ProjectTrustMatch ProjectTrustStore_Find(const ProjectTrustStore* store,
                                         StrView path);
ProjectTrustResolution ProjectTrust_Resolve(bool hasGatedResources,
                                            ProjectTrustOverride overrideValue,
                                            const ProjectTrustStore* store,
                                            StrView workingDirectory,
                                            DefaultProjectTrust defaultValue,
                                            bool interactive);
ProjectTrustReloadResolution ProjectTrust_ResolveReload(
    bool hasGatedResourcesNow, bool trustCoversProjectResources,
    ProjectTrustOverride overrideValue, bool currentTrusted);

// Reloads and updates the store while holding a cross-process lock. If
// `removePath` is non-empty, that exact entry is removed before writing.
bool ProjectTrustStore_Update(Arena* scratch, StrView storePath,
                              StrView decisionPath, bool trusted,
                              StrView removePath, StrBuf* error);
