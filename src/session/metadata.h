#pragma once

#include "session/entry.h"

constexpr u32 SESSION_DISPLAY_TITLE_MAX_BYTES = 96;

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
