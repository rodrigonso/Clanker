#pragma once

#include "session/entry.h"

constexpr u32 SESSION_DISPLAY_TITLE_MAX_BYTES = 96;

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
  u64 ActiveTimeMs;

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
  u32 GenerationCount;
  u32 UnknownUsageCount;
  u32 CompactionCount;
  u32 OperationCount;

  bool Busy;
  bool Running;
  bool Suspended;
  bool ActiveTimePartial;
  bool UsageIncomplete;
};

struct SessionMetadataBuildOptions {
  bool Busy;
  bool Running;
  bool Suspended;
  u64 NowMs;
};

StrView SessionMetadata_FlattenDisplayText(
    Arena* arena, StrView text,
    u32 maxBytes = SESSION_DISPLAY_TITLE_MAX_BYTES);

SessionResult SessionMetadata_Build(
    Arena* arena, const SessionRecord* records, u32 count,
    const SessionMetadataBuildOptions* options, SessionInfo* out);

void SessionInfo_SortByActivity(Array<SessionInfo>* items);
