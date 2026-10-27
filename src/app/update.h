#pragma once
#include "base/arena.h"
#include "base/str.h"
#include "base/thread.h"

// Self-update for copies installed by tools/install/install.{ps1,sh}. The
// install scripts are the single implementation of download, checksum
// verification, and replacement; `clanker upgrade` just reruns the right one
// against the running copy's install directory.

#define CLANKER_REPOSITORY "rodrigonso/Clanker"
#define CLANKER_RELEASES_API_URL \
  "https://api.github.com/repos/" CLANKER_REPOSITORY "/releases?per_page=1"
#define CLANKER_INSTALL_SCRIPT_BASE_URL \
  "https://raw.githubusercontent.com/" CLANKER_REPOSITORY "/main/tools/install/"

// "X.Y.Z" with an optional leading 'v'.
bool Version_Parse(StrView text, u32 out[3]);
// Negative, zero, or positive like strcmp. Unparsable versions compare equal,
// so garbage never produces an update notice.
i32 Version_Compare(StrView a, StrView b);

// Newest release version (without 'v') from GET /releases?per_page=1. Drafts
// are invisible to anonymous requests, and prereleases count, because every
// public-alpha release is one.
StrView Releases_ParseLatest(Arena* arena, StrView json);

// The install prefix when `exePath` is <prefix>/bin/clanker[.exe] inside an
// installed archive layout (marked by share/clanker/DISTRIBUTION.txt); empty
// for development builds.
StrView Install_PrefixFromExe(Arena* arena, StrView exePath);
StrView Install_Prefix(Arena* arena);

// `clanker upgrade [--version X.Y.Z]`.
int RunUpgrade(Arena* arena, const char* version);

// ---- update notice ------------------------------------------------------
// The check runs at most daily in the background and only records the newest
// version; the notice is shown from that record on a later start, so startup
// never waits on the network.

struct UpdateCache {
  u64 CheckedAtMs;
  StrView Latest;
};

bool UpdateCache_Parse(Arena* arena, StrView json, UpdateCache* out);
StrView UpdateCache_Format(Arena* arena, const UpdateCache* cache);
bool UpdateCache_IsStale(const UpdateCache* cache, u64 nowMs);

// Notice text when `cache` names a version newer than `current`, else empty.
StrView UpdateNotice(Arena* arena, const UpdateCache* cache, StrView current);

struct UpdateCheck {
  Thread Worker;
  bool Running;
  volatile i32 Abort;
  StrView CachePath;  // owned by the caller's arena
  UpdateCache Cached; // as read at startup; owned by the caller's arena
  StrView Notice;
};

// Reads the cache, fills `check->Notice`, and starts a background refresh when
// the cache is stale. Does nothing for development builds or when
// CLANKER_NO_UPDATE_CHECK is set. Requires Http_GlobalInit.
void UpdateCheck_Start(UpdateCheck* check, Arena* arena);
// Cancels and joins the refresh. Must run before Http_GlobalShutdown.
void UpdateCheck_Stop(UpdateCheck* check);
