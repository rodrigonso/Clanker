#include "session/metadata.h"

#include <string.h>

#include "session/reducer.h"

namespace {

bool IsAsciiWhitespace(u8 c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

bool IsContinuation(u8 c) { return (c & 0xC0u) == 0x80u; }

u32 Utf8ScalarLength(const char* data, u32 remaining) {
  if (!remaining) return 0;
  u8 a = (u8)data[0];
  if (a < 0x80u) return 1;
  if (a >= 0xC2u && a <= 0xDFu)
    return remaining >= 2 && IsContinuation((u8)data[1]) ? 2 : 0;
  if (a == 0xE0u)
    return remaining >= 3 && (u8)data[1] >= 0xA0u &&
                   (u8)data[1] <= 0xBFu &&
                   IsContinuation((u8)data[2])
               ? 3
               : 0;
  if ((a >= 0xE1u && a <= 0xECu) || (a >= 0xEEu && a <= 0xEFu))
    return remaining >= 3 && IsContinuation((u8)data[1]) &&
                   IsContinuation((u8)data[2])
               ? 3
               : 0;
  if (a == 0xEDu)
    return remaining >= 3 && (u8)data[1] >= 0x80u &&
                   (u8)data[1] <= 0x9Fu &&
                   IsContinuation((u8)data[2])
               ? 3
               : 0;
  if (a == 0xF0u)
    return remaining >= 4 && (u8)data[1] >= 0x90u &&
                   (u8)data[1] <= 0xBFu &&
                   IsContinuation((u8)data[2]) &&
                   IsContinuation((u8)data[3])
               ? 4
               : 0;
  if (a >= 0xF1u && a <= 0xF3u)
    return remaining >= 4 && IsContinuation((u8)data[1]) &&
                   IsContinuation((u8)data[2]) &&
                   IsContinuation((u8)data[3])
               ? 4
               : 0;
  if (a == 0xF4u)
    return remaining >= 4 && (u8)data[1] >= 0x80u &&
                   (u8)data[1] <= 0x8Fu &&
                   IsContinuation((u8)data[2]) &&
                   IsContinuation((u8)data[3])
               ? 4
               : 0;
  return 0;
}

u64 AddSaturated(u64 a, u64 b) {
  return ~0ull - a < b ? ~0ull : a + b;
}

u64 Elapsed(u64 start, u64 end) { return end > start ? end - start : 0; }

const SessionRecord* FindEntry(const SessionRecord* records, u32 before,
                               DurableId entryId) {
  if (!DurableId_IsSet(entryId)) return nullptr;
  for (u32 i = before; i > 0; i--)
    if (DurableId_Eq(records[i - 1].EntryId, entryId))
      return &records[i - 1];
  return nullptr;
}

bool HasGenerationForMessage(const SessionRecord* records, u32 start,
                             u32 count, DurableId messageEntryId) {
  for (u32 i = start; i < count; i++) {
    if (records[i].Kind == SessionRecordKind::GenerationFinished &&
        DurableId_Eq(records[i].GenerationFinished.MessageEntryId,
                     messageEntryId))
      return true;
  }
  return false;
}

u64 NormalizedTokens(const Usage* usage) {
  if (!usage) return 0;
  if (usage->TotalTokens) return usage->TotalTokens;
  u64 total = usage->Input;
  total = AddSaturated(total, usage->CacheRead);
  total = AddSaturated(total, usage->CacheWrite);
  return AddSaturated(total, usage->Output);
}

void AddUsage(SessionInfo* info, const Usage* usage) {
  info->InputTokens = AddSaturated(info->InputTokens, usage->Input);
  info->OutputTokens = AddSaturated(info->OutputTokens, usage->Output);
  info->CacheReadTokens =
      AddSaturated(info->CacheReadTokens, usage->CacheRead);
  info->CacheWriteTokens =
      AddSaturated(info->CacheWriteTokens, usage->CacheWrite);
  info->ReasoningTokens =
      AddSaturated(info->ReasoningTokens, usage->Reasoning);
  info->TotalTokens =
      AddSaturated(info->TotalTokens, NormalizedTokens(usage));
}

StrView FirstUserText(Arena* arena, const SessionRecord* records, u32 count) {
  for (u32 i = 0; i < count; i++) {
    if (records[i].Kind != SessionRecordKind::Message ||
        records[i].MessageEntry.Value.Kind != Role::User)
      continue;
    const Message* message = &records[i].MessageEntry.Value;
    StrBuf text = StrBuf_Make(arena, 128);
    for (u32 c = 0; c < message->Content.Count; c++) {
      const Content* content = &message->Content.Data[c];
      if (content->Kind != ContentKind::Text || !content->Text.Length) continue;
      if (text.Length) StrBuf_AppendChar(&text, ' ');
      StrBuf_Append(&text, content->Text);
    }
    if (!text.Length) continue;
    return SessionMetadata_FlattenDisplayText(arena, StrBuf_View(&text), 0);
  }
  return {};
}

}  // namespace

StrView SessionMetadata_FlattenDisplayText(Arena* arena, StrView text,
                                           u32 maxBytes) {
  if (!arena || !text.Data || !text.Length) return {};
  StrBuf result =
      StrBuf_Make(arena, maxBytes ? maxBytes + 1 : text.Length + 1);
  bool pendingSpace = false;
  for (u32 i = 0; i < text.Length;) {
    u8 c = (u8)text.Data[i];
    if (c < 0x80u) {
      i++;
      if (IsAsciiWhitespace(c)) {
        if (result.Length) pendingSpace = true;
        continue;
      }
      if (c < 0x20u || c == 0x7Fu) continue;
      u32 needed = (pendingSpace ? 1u : 0u) + 1u;
      if (maxBytes && result.Length + needed > maxBytes) break;
      if (pendingSpace) StrBuf_AppendChar(&result, ' ');
      pendingSpace = false;
      StrBuf_AppendChar(&result, (char)c);
      continue;
    }

    u32 scalar = Utf8ScalarLength(text.Data + i, text.Length - i);
    if (!scalar) {
      i++;
      continue;
    }
    u32 needed = (pendingSpace ? 1u : 0u) + scalar;
    if (maxBytes && result.Length + needed > maxBytes) break;
    if (pendingSpace) StrBuf_AppendChar(&result, ' ');
    pendingSpace = false;
    StrBuf_Append(&result, StrView{text.Data + i, scalar});
    i += scalar;
  }
  return StrBuf_View(&result);
}

SessionResult SessionMetadata_Build(
    Arena* arena, const SessionRecord* records, u32 count,
    const SessionMetadataBuildOptions* options, SessionInfo* out) {
  if (!arena || !records || !count || !out)
    return SessionResult_Error(SessionStatus::InvalidArgument,
                               SVL("session metadata arguments are invalid"));
  Arena scratch = ArenaCreate();
  SessionReduction reduction = {};
  SessionResult reduced =
      SessionReducer_Reduce(&scratch, records, count, &reduction);
  if (!SessionResult_IsOk(reduced)) {
    ArenaRelease(&scratch);
    return reduced;
  }

  SessionInfo info = {};
  const SessionRecord* started = &records[0];
  info.SessionId = started->SessionId;
  info.ProjectKey =
      StrView_Dup(arena, started->SessionStarted.ProjectKey);
  info.WorkingDirectory =
      StrView_Dup(arena, started->SessionStarted.WorkingDirectory);
  info.ForkedFromSessionId =
      started->SessionStarted.ForkedFromSessionId;
  info.ForkedFromEntryId = started->SessionStarted.ForkedFromEntryId;
  info.CreatedAt = started->CreatedAt;
  info.StorageUpdatedAt = records[count - 1].CreatedAt;
  info.LastActivityAt = info.CreatedAt;
  info.Busy = options && options->Busy;

  StrView explicitName = {};
  StrView firstUserText = {};
  StrView label = started->SessionStarted.Label;
  StrView latestProvider = {};
  StrView latestModel = {};
  const SessionRecord* pendingOperation = nullptr;
  u64 pendingOperationActivityAt = 0;
  u64 completedActiveMs = 0;
  for (u32 i = 0; i < count; i++) {
    const SessionRecord* record = &records[i];
    if (pendingOperation &&
        record->Kind != SessionRecordKind::SessionName &&
        record->Kind != SessionRecordKind::BranchLabel)
      pendingOperationActivityAt = record->CreatedAt;
    switch (record->Kind) {
      case SessionRecordKind::SessionName:
        explicitName = record->SessionNameEntry.Name;
        break;
      case SessionRecordKind::Message: {
        info.MessageCount++;
        const Message* message = &record->MessageEntry.Value;
        if (message->Kind == Role::User) {
          info.UserMessageCount++;
          if (!firstUserText.Length)
            firstUserText = FirstUserText(arena, record, 1);
        } else if (message->Kind == Role::Assistant) {
          info.AssistantMessageCount++;
          for (u32 c = 0; c < message->Content.Count; c++)
            if (message->Content.Data[c].Kind == ContentKind::ToolCall)
              info.ToolCallCount++;
          if (!HasGenerationForMessage(records, i + 1, count,
                                       record->EntryId))
            AddUsage(&info, &message->Use);
          if (message->Provider.Length)
            latestProvider = message->Provider;
          if (message->Model.Length)
            latestModel = message->Model;
        } else {
          info.ToolResultMessageCount++;
        }
        info.LastActivityAt = record->CreatedAt;
        break;
      }
      case SessionRecordKind::Model:
        break;
      case SessionRecordKind::BranchLabel:
        break;
      case SessionRecordKind::Compaction:
        info.CompactionCount++;
        info.LastActivityAt = record->CreatedAt;
        break;
      case SessionRecordKind::GenerationStarted:
        info.GenerationCount++;
        latestProvider = record->GenerationStarted.Provider;
        latestModel = record->GenerationStarted.Model;
        break;
      case SessionRecordKind::GenerationFinished:
        if (record->GenerationFinished.HasUsage) {
          AddUsage(&info, &record->GenerationFinished.Use);
        } else {
          const SessionRecord* message =
              FindEntry(records, i,
                        record->GenerationFinished.MessageEntryId);
          if (message && message->Kind == SessionRecordKind::Message &&
              message->MessageEntry.Value.Kind == Role::Assistant)
            AddUsage(&info, &message->MessageEntry.Value.Use);
          else
            info.UnknownUsageCount++;
        }
        break;
      case SessionRecordKind::OperationStarted:
        info.OperationCount++;
        pendingOperation = record;
        pendingOperationActivityAt = record->CreatedAt;
        break;
      case SessionRecordKind::OperationFinished:
      case SessionRecordKind::OperationAborted:
        if (pendingOperation) {
          completedActiveMs = AddSaturated(
              completedActiveMs,
              Elapsed(pendingOperation->CreatedAt, record->CreatedAt));
          pendingOperation = nullptr;
          pendingOperationActivityAt = 0;
        }
        break;
      default:
        break;
    }
  }

  for (u32 i = 0; i < reduction.Branch.Count; i++) {
    const SessionRecord* record = reduction.Branch.Data[i];
    if (record->Kind == SessionRecordKind::BranchLabel)
      label = record->BranchLabelEntry.Label;
  }
  info.ExplicitName = StrView_Dup(arena, explicitName);
  info.Label = StrView_Dup(arena, label);
  info.CurrentProvider = StrView_Dup(arena, reduction.Provider);
  info.CurrentModel = StrView_Dup(arena, reduction.Model);
  if (!info.CurrentProvider.Length)
    info.CurrentProvider = StrView_Dup(arena, latestProvider);
  if (!info.CurrentModel.Length)
    info.CurrentModel = StrView_Dup(arena, latestModel);

  StrView titleSource =
      info.ExplicitName.Length ? info.ExplicitName : firstUserText;
  info.DisplayTitle = SessionMetadata_FlattenDisplayText(
      arena, titleSource, SESSION_DISPLAY_TITLE_MAX_BYTES);

  info.WallSpanMs = Elapsed(info.CreatedAt, info.LastActivityAt);
  u64 pendingActiveMs = 0;
  if (pendingOperation) {
    u64 end = pendingOperationActivityAt
                  ? pendingOperationActivityAt
                  : pendingOperation->CreatedAt;
    if (options && options->Running && options->NowMs > end)
      end = options->NowMs;
    pendingActiveMs = Elapsed(pendingOperation->CreatedAt, end);
    info.Running = options && options->Running;
    info.Suspended =
        options && options->Suspended;
    if (!info.Running && !info.Busy)
      info.Suspended = true;
    info.ActiveTimePartial = !info.Running;
  }
  info.ActiveTimeMs =
      AddSaturated(completedActiveMs, pendingActiveMs);
  info.UsageIncomplete = info.UnknownUsageCount > 0;

  *out = info;
  ArenaRelease(&scratch);
  return SessionResult_Ok(records[count - 1].Seq);
}

void SessionInfo_SortByActivity(Array<SessionInfo>* items) {
  if (!items) return;
  for (u32 i = 1; i < items->Count; i++) {
    SessionInfo value = items->Data[i];
    u32 j = i;
    while (j > 0) {
      const SessionInfo* previous = &items->Data[j - 1];
      bool before = previous->LastActivityAt < value.LastActivityAt ||
                    (previous->LastActivityAt == value.LastActivityAt &&
                     previous->CreatedAt < value.CreatedAt) ||
                    (previous->LastActivityAt == value.LastActivityAt &&
                     previous->CreatedAt == value.CreatedAt &&
                     memcmp(previous->SessionId.Value, value.SessionId.Value,
                            DURABLE_ID_CHAR_COUNT) > 0);
      if (!before) break;
      items->Data[j] = items->Data[j - 1];
      j--;
    }
    items->Data[j] = value;
  }
}
