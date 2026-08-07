#pragma once

#include "session/entry.h"

constexpr u32 SESSION_DISPLAY_TITLE_MAX_BYTES = 96;

// A project key doubles as the storage directory name, so two keys that name
// the same directory have to compare equal. Windows filesystems are
// case-insensitive, and the key generator folds case there for exactly that
// reason; a session recorded before that folding existed still has to match the
// folded key it is now looked up by.
inline bool SessionProjectKey_Eq(StrView a, StrView b) {
#if defined(_WIN32)
  if (a.Length != b.Length) return false;
  for (u32 i = 0; i < a.Length; i++) {
    char x = a.Data[i];
    char y = b.Data[i];
    if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
#else
  return StrView_Eq(a, b);
#endif
}

// Derived, read-only view of a session for listing/display purposes. Every
// field here is recomputed from the record log (or supplied by the caller
// for the transient Busy/NowMs signal); v2 has no durable operation/recovery
// state, so there is nothing to reconcile against.
struct SessionInfo {
  DurableId SessionId;
  StrView ProjectKey;
  StrView WorkingDirectory;
  StrView ExplicitName;
  StrView DisplayTitle;
  StrView Label;
  DurableId ForkedFromSessionId;
  DurableId ForkedFromEntryId;

  StrView CurrentProvider;
  StrView CurrentModel;

  u64 CreatedAt;
  u64 StorageUpdatedAt;
  u64 LastActivityAt;
  u64 WallSpanMs;

  u64 InputTokens;
  u64 OutputTokens;
  u64 CacheReadTokens;
  u64 CacheWriteTokens;
  u64 ReasoningTokens;
  u64 TotalTokens;

  u32 MessageCount;
  u32 UserMessageCount;
  u32 AssistantMessageCount;
  u32 ToolResultMessageCount;
  u32 ToolCallCount;
  u32 CompactionCount;

  // Transient: whether the harness currently has an in-memory turn running
  // for this session. Never derived from (or persisted to) the record log.
  bool Busy;
};

struct SessionMetadataBuildOptions {
  bool Busy;
  u64 NowMs;  // when Busy, extends WallSpanMs up to "now"
};

StrView SessionMetadata_FlattenDisplayText(
    Arena* arena, StrView text,
    u32 maxBytes = SESSION_DISPLAY_TITLE_MAX_BYTES);

SessionResult SessionMetadata_Build(
    Arena* arena, const SessionRecord* records, u32 count,
    const SessionMetadataBuildOptions* options, SessionInfo* out);

void SessionInfo_SortByActivity(Array<SessionInfo>* items);
