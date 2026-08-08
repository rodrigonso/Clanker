#include "session/entry.h"

#include <string.h>

#include "json/json.h"

namespace {

constexpr f64 JSON_SAFE_INTEGER_MAX = 9007199254740991.0;

// Durable-record extensions over the shared JsonWriter in json/json.h.
void WriteId(JsonWriter* w, const char* key, DurableId id) {
  JsonWrite_Key(w, key);
  Json_WriteString(w->Out, DurableId_View(&id));
}

void WriteOptionalId(JsonWriter* w, const char* key, DurableId id) {
  JsonWrite_Key(w, key);
  if (DurableId_IsSet(id))
    Json_WriteString(w->Out, DurableId_View(&id));
  else
    StrBuf_AppendCStr(w->Out, "null");
}

StrView RoleName(Role value) {
  switch (value) {
    case Role::User: return SVL("user");
    case Role::Assistant: return SVL("assistant");
    case Role::ToolResult: return SVL("tool_result");
  }
  return StrView{};
}

bool ParseRole(StrView text, Role* out) {
  if (StrView_Eq(text, SVL("user"))) {
    *out = Role::User;
    return true;
  }
  if (StrView_Eq(text, SVL("assistant"))) {
    *out = Role::Assistant;
    return true;
  }
  if (StrView_Eq(text, SVL("tool_result"))) {
    *out = Role::ToolResult;
    return true;
  }
  return false;
}

StrView StopReasonName(StopReason value) {
  switch (value) {
    case StopReason::Stop: return SVL("stop");
    case StopReason::Length: return SVL("length");
    case StopReason::ToolUse: return SVL("tool_use");
    case StopReason::Error: return SVL("error");
    case StopReason::Aborted: return SVL("aborted");
  }
  return StrView{};
}

bool ParseStopReason(StrView text, StopReason* out) {
  if (StrView_Eq(text, SVL("stop"))) {
    *out = StopReason::Stop;
    return true;
  }
  if (StrView_Eq(text, SVL("length"))) {
    *out = StopReason::Length;
    return true;
  }
  if (StrView_Eq(text, SVL("tool_use"))) {
    *out = StopReason::ToolUse;
    return true;
  }
  if (StrView_Eq(text, SVL("error"))) {
    *out = StopReason::Error;
    return true;
  }
  if (StrView_Eq(text, SVL("aborted"))) {
    *out = StopReason::Aborted;
    return true;
  }
  return false;
}

StrView ContentKindName(ContentKind value) {
  switch (value) {
    case ContentKind::Text: return SVL("text");
    case ContentKind::Thinking: return SVL("thinking");
    case ContentKind::ToolCall: return SVL("tool_call");
    case ContentKind::Image: return SVL("image");
  }
  return StrView{};
}

bool ParseContentKind(StrView text, ContentKind* out) {
  if (StrView_Eq(text, SVL("text"))) {
    *out = ContentKind::Text;
    return true;
  }
  if (StrView_Eq(text, SVL("thinking"))) {
    *out = ContentKind::Thinking;
    return true;
  }
  if (StrView_Eq(text, SVL("tool_call"))) {
    *out = ContentKind::ToolCall;
    return true;
  }
  if (StrView_Eq(text, SVL("image"))) {
    *out = ContentKind::Image;
    return true;
  }
  return false;
}

bool ParseRecordKind(StrView text, SessionRecordKind* out) {
  for (u32 i = 0; i <= (u32)SessionRecordKind::Leaf; i++) {
    SessionRecordKind kind = (SessionRecordKind)i;
    if (StrView_Eq(text, SessionRecordKind_Name(kind))) {
      *out = kind;
      return true;
    }
  }
  return false;
}

bool ReadRequiredString(const JsonValue* object, const char* key, bool allowEmpty,
                        StrView* out) {
  JsonValue* value = Json_Get(object, key);
  if (!Json_IsString(value) || (!allowEmpty && value->Str.Length == 0)) return false;
  *out = value->Str;
  return true;
}

bool ReadRequiredBool(const JsonValue* object, const char* key, bool* out) {
  JsonValue* value = Json_Get(object, key);
  if (!value || value->Kind != JsonKind::Bool) return false;
  *out = value->Bool;
  return true;
}

bool ReadRequiredU64(const JsonValue* object, const char* key, u64* out) {
  JsonValue* value = Json_Get(object, key);
  if (!value || value->Kind != JsonKind::Number || value->Number < 0 ||
      value->Number > JSON_SAFE_INTEGER_MAX) {
    return false;
  }
  u64 converted = (u64)value->Number;
  if ((f64)converted != value->Number) return false;
  *out = converted;
  return true;
}

bool ReadRequiredId(const JsonValue* object, const char* key, DurableId* out) {
  JsonValue* value = Json_Get(object, key);
  return Json_IsString(value) && DurableId_Parse(value->Str, out);
}

bool ReadOptionalString(const JsonValue* object, const char* key,
                        StrView* out) {
  JsonValue* value = Json_Get(object, key);
  if (!value || value->Kind == JsonKind::Null) {
    *out = {};
    return true;
  }
  if (!Json_IsString(value)) return false;
  *out = value->Str;
  return true;
}

bool ReadOptionalId(const JsonValue* object, const char* key, DurableId* out) {
  JsonValue* value = Json_Get(object, key);
  if (!value || value->Kind == JsonKind::Null) {
    *out = DurableId{};
    return value != nullptr;
  }
  return Json_IsString(value) && DurableId_Parse(value->Str, out);
}

void WriteContent(StrBuf* out, const Content* content) {
  JsonWriter w;
  JsonWrite_Begin(&w, out);
  JsonWrite_String(&w, "kind", ContentKindName(content->Kind));
  switch (content->Kind) {
    case ContentKind::Text:
      JsonWrite_String(&w, "text", content->Text);
      JsonWrite_String(&w, "signature", content->Signature);
      break;
    case ContentKind::Thinking:
      JsonWrite_String(&w, "text", content->Text);
      JsonWrite_String(&w, "signature", content->Signature);
      JsonWrite_Bool(&w, "redacted", content->Redacted);
      break;
    case ContentKind::ToolCall:
      JsonWrite_String(&w, "toolId", content->ToolId);
      JsonWrite_String(&w, "toolName", content->ToolName);
      JsonWrite_String(&w, "argumentsJson", content->ToolArgsJson);
      if (content->Signature.Length)
        JsonWrite_String(&w, "signature", content->Signature);
      break;
    case ContentKind::Image:
      JsonWrite_String(&w, "data", content->ImageData);
      JsonWrite_String(&w, "mimeType", content->ImageMimeType);
      break;
  }
  JsonWrite_End(&w);
}

void WriteUsage(StrBuf* out, const Usage* usage) {
  JsonWriter w;
  JsonWrite_Begin(&w, out);
  JsonWrite_U64(&w, "input", usage->Input);
  JsonWrite_U64(&w, "output", usage->Output);
  JsonWrite_U64(&w, "cacheRead", usage->CacheRead);
  JsonWrite_U64(&w, "cacheWrite", usage->CacheWrite);
  JsonWrite_U64(&w, "reasoning", usage->Reasoning);
  JsonWrite_U64(&w, "totalTokens", usage->TotalTokens);
  JsonWrite_End(&w);
}

void WriteMessage(StrBuf* out, const Message* message) {
  JsonWriter w;
  JsonWrite_Begin(&w, out);
  JsonWrite_String(&w, "role", RoleName(message->Kind));
  JsonWrite_Key(&w, "content");
  StrBuf_AppendChar(out, '[');
  for (u32 i = 0; i < message->Content.Count; i++) {
    if (i) StrBuf_AppendChar(out, ',');
    WriteContent(out, &message->Content.Data[i]);
  }
  StrBuf_AppendChar(out, ']');
  JsonWrite_U64(&w, "timestamp", message->Timestamp);
  if (message->Kind == Role::Assistant) {
    JsonWrite_String(&w, "stopReason", StopReasonName(message->Stop));
    JsonWrite_Key(&w, "usage");
    WriteUsage(out, &message->Use);
    JsonWrite_String(&w, "errorMessage", message->ErrorMessage);
    JsonWrite_String(&w, "model", message->Model);
    JsonWrite_String(&w, "provider", message->Provider);
  } else if (message->Kind == Role::ToolResult) {
    JsonWrite_String(&w, "toolCallId", message->ToolCallId);
    JsonWrite_String(&w, "toolName", message->ToolName);
    JsonWrite_Bool(&w, "isError", message->IsError);
  }
  JsonWrite_End(&w);
}

// Decoded strings are views into the JSON document, which the caller's arena
// already owns, so nothing here allocates.
bool DecodeContent(const JsonValue* object, Content* out) {
  if (!Json_IsObject(object)) return false;
  Content result = {};
  if (!ParseContentKind(Json_GetStr(object, "kind"), &result.Kind)) return false;
  switch (result.Kind) {
    case ContentKind::Text:
      if (!ReadRequiredString(object, "text", true, &result.Text) ||
          !ReadRequiredString(object, "signature", true, &result.Signature)) {
        return false;
      }
      break;
    case ContentKind::Thinking:
      if (!ReadRequiredString(object, "text", true, &result.Text) ||
          !ReadRequiredString(object, "signature", true, &result.Signature) ||
          !ReadRequiredBool(object, "redacted", &result.Redacted)) {
        return false;
      }
      break;
    case ContentKind::ToolCall: {
      if (!ReadRequiredString(object, "toolId", false, &result.ToolId) ||
          !ReadRequiredString(object, "toolName", false, &result.ToolName) ||
          !ReadRequiredString(object, "argumentsJson", false,
                              &result.ToolArgsJson) ||
          !ReadOptionalString(object, "signature", &result.Signature)) {
        return false;
      }
      break;
    }
    case ContentKind::Image:
      if (!ReadRequiredString(object, "data", false, &result.ImageData) ||
          !ReadRequiredString(object, "mimeType", false, &result.ImageMimeType)) {
        return false;
      }
      break;
  }
  *out = result;
  return true;
}

bool DecodeUsage(const JsonValue* object, Usage* out) {
  if (!Json_IsObject(object)) return false;
  u64 input = 0;
  u64 output = 0;
  u64 cacheRead = 0;
  u64 cacheWrite = 0;
  u64 reasoning = 0;
  u64 total = 0;
  if (!ReadRequiredU64(object, "input", &input) || input > 0xFFFFFFFFull ||
      !ReadRequiredU64(object, "output", &output) || output > 0xFFFFFFFFull ||
      !ReadRequiredU64(object, "cacheRead", &cacheRead) || cacheRead > 0xFFFFFFFFull ||
      !ReadRequiredU64(object, "cacheWrite", &cacheWrite) || cacheWrite > 0xFFFFFFFFull ||
      !ReadRequiredU64(object, "reasoning", &reasoning) || reasoning > 0xFFFFFFFFull ||
      !ReadRequiredU64(object, "totalTokens", &total) || total > 0xFFFFFFFFull) {
    return false;
  }
  out->Input = (u32)input;
  out->Output = (u32)output;
  out->CacheRead = (u32)cacheRead;
  out->CacheWrite = (u32)cacheWrite;
  out->Reasoning = (u32)reasoning;
  out->TotalTokens = (u32)total;
  return true;
}

bool DecodeMessage(Arena* arena, const JsonValue* object, Message* out) {
  if (!Json_IsObject(object)) return false;
  Message message = {};
  if (!ParseRole(Json_GetStr(object, "role"), &message.Kind)) return false;
  JsonValue* content = Json_Get(object, "content");
  if (!Json_IsArray(content)) return false;
  message.Content = Array_Make<Content>(arena, content->Count ? content->Count : 1);
  for (u32 i = 0; i < content->Count; i++) {
    Content item = {};
    if (!DecodeContent(content->Items[i], &item)) return false;
    Array_Push(&message.Content, item);
  }
  if (!ReadRequiredU64(object, "timestamp", &message.Timestamp)) return false;
  if (message.Kind == Role::User) {
    if (message.Content.Count == 0) return false;
  } else if (message.Kind == Role::Assistant) {
    if (!ParseStopReason(Json_GetStr(object, "stopReason"), &message.Stop) ||
        !DecodeUsage(Json_Get(object, "usage"), &message.Use) ||
        !ReadRequiredString(object, "errorMessage", true, &message.ErrorMessage) ||
        !ReadRequiredString(object, "model", true, &message.Model) ||
        !ReadRequiredString(object, "provider", true, &message.Provider)) {
      return false;
    }
  } else {
    if (message.Content.Count == 0 ||
        !ReadRequiredString(object, "toolCallId", false, &message.ToolCallId) ||
        !ReadRequiredString(object, "toolName", false, &message.ToolName) ||
        !ReadRequiredBool(object, "isError", &message.IsError)) {
      return false;
    }
  }
  *out = message;
  return true;
}

bool MessageValid(const Message* message) {
  if (!message || message->Content.Count > 0x100000u) return false;
  switch (message->Kind) {
    case Role::User:
      if (message->Content.Count == 0) return false;
      break;
    case Role::Assistant:
      if (StopReasonName(message->Stop).Length == 0) return false;
      break;
    case Role::ToolResult:
      if (message->Content.Count == 0 || message->ToolCallId.Length == 0 ||
          message->ToolName.Length == 0) {
        return false;
      }
      break;
    default: return false;
  }
  for (u32 i = 0; i < message->Content.Count; i++) {
    const Content* content = &message->Content.Data[i];
    if (ContentKindName(content->Kind).Length == 0) return false;
    if (content->Kind == ContentKind::ToolCall &&
        (content->ToolId.Length == 0 || content->ToolName.Length == 0 ||
         content->ToolArgsJson.Length == 0)) {
      return false;
    }
    if (content->Kind == ContentKind::Image &&
        (content->ImageData.Length == 0 || content->ImageMimeType.Length == 0)) {
      return false;
    }
  }
  return true;
}

Content CloneContent(Arena* arena, const Content& source) {
  Content result = source;
  result.Text = StrView_Dup(arena, source.Text);
  result.Signature = StrView_Dup(arena, source.Signature);
  result.ToolId = StrView_Dup(arena, source.ToolId);
  result.ToolName = StrView_Dup(arena, source.ToolName);
  result.ToolArgsJson = StrView_Dup(arena, source.ToolArgsJson);
  result.ImageData = StrView_Dup(arena, source.ImageData);
  result.ImageMimeType = StrView_Dup(arena, source.ImageMimeType);
  return result;
}

Array<Content> CloneContentArray(Arena* arena, const Array<Content>& source) {
  Array<Content> result =
      Array_Make<Content>(arena, source.Count ? source.Count : 1);
  for (u32 i = 0; i < source.Count; i++)
    Array_Push(&result, CloneContent(arena, source.Data[i]));
  return result;
}

Array<StrView> CloneStringArray(Arena* arena, const Array<StrView>& source) {
  Array<StrView> cloned =
      Array_Make<StrView>(arena, source.Count ? source.Count : 1);
  for (u32 i = 0; i < source.Count; i++)
    Array_Push(&cloned, StrView_Dup(arena, source.Data[i]));
  return cloned;
}

Message CloneMessage(Arena* arena, const Message& source) {
  Message result = source;
  result.Content = CloneContentArray(arena, source.Content);
  result.ErrorMessage = StrView_Dup(arena, source.ErrorMessage);
  result.Model = StrView_Dup(arena, source.Model);
  result.Provider = StrView_Dup(arena, source.Provider);
  result.ToolCallId = StrView_Dup(arena, source.ToolCallId);
  result.ToolName = StrView_Dup(arena, source.ToolName);
  return result;
}

void WriteStringArray(StrBuf* out, const Array<StrView>* values) {
  StrBuf_AppendChar(out, '[');
  for (u32 i = 0; i < values->Count; i++) {
    if (i) StrBuf_AppendChar(out, ',');
    Json_WriteString(out, values->Data[i]);
  }
  StrBuf_AppendChar(out, ']');
}

// Decodes an optional array of non-empty strings. A missing key yields an empty
// array so records written before the field existed stay readable.
bool ReadOptionalStringArray(Arena* arena, const JsonValue* payload, const char* key,
                             Array<StrView>* out) {
  *out = Array_Make<StrView>(arena, 1);
  JsonValue* values = Json_Get(payload, key);
  if (!values) return true;
  if (!Json_IsArray(values)) return false;
  if (values->Count) *out = Array_Make<StrView>(arena, values->Count);
  for (u32 i = 0; i < values->Count; i++) {
    if (!Json_IsString(values->Items[i]) || values->Items[i]->Str.Length == 0) return false;
    Array_Push(out, values->Items[i]->Str);
  }
  return true;
}

void WritePayload(StrBuf* out, const SessionRecord* record) {
  JsonWriter w;
  JsonWrite_Begin(&w, out);
  switch (record->Kind) {
    case SessionRecordKind::SessionStarted:
      JsonWrite_String(&w, "workingDirectory", record->SessionStarted.WorkingDirectory);
      JsonWrite_String(&w, "projectKey", record->SessionStarted.ProjectKey);
      JsonWrite_String(&w, "label", record->SessionStarted.Label);
      WriteOptionalId(&w, "forkedFromSessionId", record->SessionStarted.ForkedFromSessionId);
      WriteOptionalId(&w, "forkedFromEntryId", record->SessionStarted.ForkedFromEntryId);
      break;
    case SessionRecordKind::Message:
      WriteId(&w, "parentId", record->MessageEntry.ParentId);
      JsonWrite_Key(&w, "message");
      WriteMessage(out, &record->MessageEntry.Value);
      break;
    case SessionRecordKind::Model:
      WriteId(&w, "parentId", record->ModelEntry.ParentId);
      JsonWrite_String(&w, "provider", record->ModelEntry.Provider);
      JsonWrite_String(&w, "model", record->ModelEntry.Model);
      break;
    case SessionRecordKind::Thinking:
      WriteId(&w, "parentId", record->ThinkingEntry.ParentId);
      JsonWrite_String(&w, "level", record->ThinkingEntry.Level);
      break;
    case SessionRecordKind::Tools:
      WriteId(&w, "parentId", record->ToolsEntry.ParentId);
      JsonWrite_Key(&w, "toolNames");
      WriteStringArray(out, &record->ToolsEntry.ToolNames);
      break;
    case SessionRecordKind::Compaction:
      WriteId(&w, "parentId", record->CompactionEntry.ParentId);
      WriteId(&w, "baseEntryId", record->CompactionEntry.BaseEntryId);
      WriteId(&w, "throughEntryId", record->CompactionEntry.ThroughEntryId);
      WriteId(&w, "sourceHeadId", record->CompactionEntry.SourceHeadId);
      JsonWrite_Key(&w, "summary");
      WriteMessage(out, &record->CompactionEntry.Summary);
      if (record->CompactionEntry.HasUsage) {
        JsonWrite_Key(&w, "usage");
        WriteUsage(out, &record->CompactionEntry.Use);
      }
      if (record->CompactionEntry.ReadFiles.Count) {
        JsonWrite_Key(&w, "readFiles");
        WriteStringArray(out, &record->CompactionEntry.ReadFiles);
      }
      if (record->CompactionEntry.ModifiedFiles.Count) {
        JsonWrite_Key(&w, "modifiedFiles");
        WriteStringArray(out, &record->CompactionEntry.ModifiedFiles);
      }
      break;
    case SessionRecordKind::BranchLabel:
      WriteId(&w, "parentId", record->BranchLabelEntry.ParentId);
      JsonWrite_String(&w, "label", record->BranchLabelEntry.Label);
      break;
    case SessionRecordKind::SessionName:
      JsonWrite_String(&w, "name", record->SessionNameEntry.Name);
      break;
    case SessionRecordKind::Leaf:
      WriteId(&w, "leafId", record->LeafEntry.LeafId);
      break;
  }
  JsonWrite_End(&w);
}

bool DecodePayload(Arena* arena, const JsonValue* payload, SessionRecord* record) {
  if (!Json_IsObject(payload)) return false;
  switch (record->Kind) {
    case SessionRecordKind::SessionStarted:
      return ReadRequiredString(payload, "workingDirectory", false,
                                &record->SessionStarted.WorkingDirectory) &&
             ReadRequiredString(payload, "projectKey", false,
                                &record->SessionStarted.ProjectKey) &&
             ReadRequiredString(payload, "label", true, &record->SessionStarted.Label) &&
             ReadOptionalId(payload, "forkedFromSessionId",
                            &record->SessionStarted.ForkedFromSessionId) &&
             ReadOptionalId(payload, "forkedFromEntryId",
                            &record->SessionStarted.ForkedFromEntryId);
    case SessionRecordKind::Message:
      return ReadRequiredId(payload, "parentId", &record->MessageEntry.ParentId) &&
             DecodeMessage(arena, Json_Get(payload, "message"), &record->MessageEntry.Value);
    case SessionRecordKind::Model:
      return ReadRequiredId(payload, "parentId", &record->ModelEntry.ParentId) &&
             ReadRequiredString(payload, "provider", false, &record->ModelEntry.Provider) &&
             ReadRequiredString(payload, "model", false, &record->ModelEntry.Model);
    case SessionRecordKind::Thinking:
      return ReadRequiredId(payload, "parentId", &record->ThinkingEntry.ParentId) &&
             ReadRequiredString(payload, "level", false, &record->ThinkingEntry.Level);
    case SessionRecordKind::Tools: {
      if (!ReadRequiredId(payload, "parentId", &record->ToolsEntry.ParentId)) return false;
      JsonValue* names = Json_Get(payload, "toolNames");
      if (!Json_IsArray(names)) return false;
      record->ToolsEntry.ToolNames =
          Array_Make<StrView>(arena, names->Count ? names->Count : 1);
      for (u32 i = 0; i < names->Count; i++) {
        if (!Json_IsString(names->Items[i]) || names->Items[i]->Str.Length == 0) return false;
        Array_Push(&record->ToolsEntry.ToolNames, names->Items[i]->Str);
      }
      return true;
    }
    case SessionRecordKind::Compaction: {
      if (!ReadRequiredId(payload, "parentId", &record->CompactionEntry.ParentId) ||
          !ReadRequiredId(payload, "baseEntryId",
                          &record->CompactionEntry.BaseEntryId) ||
          !ReadRequiredId(payload, "throughEntryId",
                          &record->CompactionEntry.ThroughEntryId) ||
          !ReadRequiredId(payload, "sourceHeadId",
                          &record->CompactionEntry.SourceHeadId) ||
          !DecodeMessage(arena, Json_Get(payload, "summary"),
                        &record->CompactionEntry.Summary)) {
        return false;
      }
      JsonValue* usage = Json_Get(payload, "usage");
      if (usage) {
        record->CompactionEntry.HasUsage = true;
        if (!DecodeUsage(usage, &record->CompactionEntry.Use)) return false;
      }
      return ReadOptionalStringArray(arena, payload, "readFiles",
                                     &record->CompactionEntry.ReadFiles) &&
             ReadOptionalStringArray(arena, payload, "modifiedFiles",
                                     &record->CompactionEntry.ModifiedFiles);
    }
    case SessionRecordKind::BranchLabel:
      return ReadRequiredId(payload, "parentId", &record->BranchLabelEntry.ParentId) &&
             ReadRequiredString(payload, "label", false, &record->BranchLabelEntry.Label);
    case SessionRecordKind::SessionName:
      return ReadRequiredString(payload, "name", true,
                                &record->SessionNameEntry.Name);
    case SessionRecordKind::Leaf:
      return ReadRequiredId(payload, "leafId", &record->LeafEntry.LeafId);
  }
  return false;
}

DurableId ParentIdFor(const SessionRecord* record) {
  switch (record->Kind) {
    case SessionRecordKind::Message: return record->MessageEntry.ParentId;
    case SessionRecordKind::Model: return record->ModelEntry.ParentId;
    case SessionRecordKind::Thinking: return record->ThinkingEntry.ParentId;
    case SessionRecordKind::Tools: return record->ToolsEntry.ParentId;
    case SessionRecordKind::Compaction: return record->CompactionEntry.ParentId;
    case SessionRecordKind::BranchLabel: return record->BranchLabelEntry.ParentId;
    default: return DurableId{};
  }
}

}  // namespace

bool SessionRecord_IsTreeNode(SessionRecordKind kind) {
  return kind != SessionRecordKind::SessionName && kind != SessionRecordKind::Leaf;
}

StrView SessionRecordKind_Name(SessionRecordKind kind) {
  switch (kind) {
    case SessionRecordKind::SessionStarted: return SVL("session_started");
    case SessionRecordKind::Message: return SVL("message");
    case SessionRecordKind::Model: return SVL("model");
    case SessionRecordKind::Thinking: return SVL("thinking");
    case SessionRecordKind::Tools: return SVL("tools");
    case SessionRecordKind::Compaction: return SVL("compaction");
    case SessionRecordKind::BranchLabel: return SVL("branch_label");
    case SessionRecordKind::SessionName: return SVL("session_name");
    case SessionRecordKind::Leaf: return SVL("leaf");
  }
  return StrView{};
}

DurableId SessionRecord_ParentId(const SessionRecord* record) {
  return record ? ParentIdFor(record) : DurableId{};
}

static DurableId RemapId(const Array<DurableId>* oldIds,
                         const Array<DurableId>* newIds, DurableId oldId) {
  if (!oldIds || !newIds || oldIds->Count != newIds->Count)
    return DurableId{};
  for (u32 i = 0; i < oldIds->Count; i++)
    if (DurableId_Eq(oldIds->Data[i], oldId)) return newIds->Data[i];
  return DurableId{};
}

SessionResult SessionRecord_PrepareForkCopy(
    const SessionRecord* source, DurableId targetSessionId, u64 targetSeq,
    u64 createdAt, const Array<DurableId>* oldIds,
    const Array<DurableId>* newIds, SessionRecord* out) {
  if (!source || !out || !DurableId_IsSet(targetSessionId) ||
      targetSeq == 0)
    return SessionResult_Invalid("fork copy arguments are invalid");

  SessionRecord copy = *source;
  copy.SchemaVersion = SESSION_SCHEMA_VERSION;
  copy.SessionId = targetSessionId;
  copy.Seq = targetSeq;
  copy.EntryId = DurableId_Generate();
  copy.CreatedAt = createdAt;

  DurableId parent =
      RemapId(oldIds, newIds, SessionRecord_ParentId(source));
  if (!DurableId_IsSet(parent))
    return SessionResult_Corrupt("fork branch parent could not be remapped");

  switch (copy.Kind) {
    case SessionRecordKind::Message: copy.MessageEntry.ParentId = parent; break;
    case SessionRecordKind::Model: copy.ModelEntry.ParentId = parent; break;
    case SessionRecordKind::Thinking: copy.ThinkingEntry.ParentId = parent; break;
    case SessionRecordKind::Tools: copy.ToolsEntry.ParentId = parent; break;
    case SessionRecordKind::BranchLabel:
      copy.BranchLabelEntry.ParentId = parent;
      break;
    case SessionRecordKind::Compaction:
      copy.CompactionEntry.ParentId = parent;
      copy.CompactionEntry.BaseEntryId = RemapId(
          oldIds, newIds, copy.CompactionEntry.BaseEntryId);
      copy.CompactionEntry.ThroughEntryId = RemapId(
          oldIds, newIds, copy.CompactionEntry.ThroughEntryId);
      copy.CompactionEntry.SourceHeadId = RemapId(
          oldIds, newIds, copy.CompactionEntry.SourceHeadId);
      if (!DurableId_IsSet(copy.CompactionEntry.BaseEntryId) ||
          !DurableId_IsSet(copy.CompactionEntry.ThroughEntryId) ||
          !DurableId_IsSet(copy.CompactionEntry.SourceHeadId))
        return SessionResult_Corrupt("fork compaction could not be remapped");
      break;
    default:
      return SessionResult_Corrupt("fork branch contains a non-tree record");
  }

  *out = copy;
  return SessionResult_Ok(targetSeq);
}

SessionResult SessionRecord_Validate(const SessionRecord* record) {
  if (!record) return SessionResult_Invalid("record is null");
  if (record->SchemaVersion != SESSION_SCHEMA_VERSION) {
    return SessionResult_Error(
        record->SchemaVersion > SESSION_SCHEMA_VERSION ? SessionStatus::UnsupportedVersion
                                                       : SessionStatus::Corrupt,
        SVL("unsupported session schema version"));
  }
  if (!DurableId_IsSet(record->SessionId)) return SessionResult_Invalid("sessionId is required");
  if (!DurableId_IsSet(record->EntryId)) return SessionResult_Invalid("entryId is required");
  if (record->Seq == 0 || record->Seq > (u64)JSON_SAFE_INTEGER_MAX)
    return SessionResult_Invalid("seq must be a positive JSON-safe integer");
  if (record->CreatedAt > (u64)JSON_SAFE_INTEGER_MAX)
    return SessionResult_Invalid("createdAt must be a JSON-safe integer");
  if (SessionRecordKind_Name(record->Kind).Length == 0)
    return SessionResult_Invalid("record kind is invalid");

  switch (record->Kind) {
    case SessionRecordKind::SessionStarted:
      if (record->SessionStarted.WorkingDirectory.Length == 0 ||
          record->SessionStarted.ProjectKey.Length == 0)
        return SessionResult_Invalid("session metadata is incomplete");
      if (DurableId_IsSet(record->SessionStarted.ForkedFromSessionId) !=
          DurableId_IsSet(record->SessionStarted.ForkedFromEntryId))
        return SessionResult_Invalid("fork provenance must include both session and entry IDs");
      break;
    case SessionRecordKind::Message:
      if (!DurableId_IsSet(record->MessageEntry.ParentId) ||
          !MessageValid(&record->MessageEntry.Value))
        return SessionResult_Invalid("message entry is invalid");
      break;
    case SessionRecordKind::Model:
      if (!DurableId_IsSet(record->ModelEntry.ParentId) ||
          record->ModelEntry.Provider.Length == 0 || record->ModelEntry.Model.Length == 0)
        return SessionResult_Invalid("model entry is invalid");
      break;
    case SessionRecordKind::Thinking:
      if (!DurableId_IsSet(record->ThinkingEntry.ParentId) ||
          record->ThinkingEntry.Level.Length == 0)
        return SessionResult_Invalid("thinking entry is invalid");
      break;
    case SessionRecordKind::Tools:
      if (!DurableId_IsSet(record->ToolsEntry.ParentId))
        return SessionResult_Invalid("tools parentId is required");
      for (u32 i = 0; i < record->ToolsEntry.ToolNames.Count; i++)
        if (record->ToolsEntry.ToolNames.Data[i].Length == 0)
          return SessionResult_Invalid("tool names cannot be empty");
      break;
    case SessionRecordKind::Compaction:
      if (!DurableId_IsSet(record->CompactionEntry.ParentId) ||
          !DurableId_IsSet(record->CompactionEntry.BaseEntryId) ||
          !DurableId_IsSet(record->CompactionEntry.ThroughEntryId) ||
          !DurableId_IsSet(record->CompactionEntry.SourceHeadId) ||
          !MessageValid(&record->CompactionEntry.Summary))
        return SessionResult_Invalid("compaction entry is invalid");
      for (u32 i = 0; i < record->CompactionEntry.ReadFiles.Count; i++)
        if (record->CompactionEntry.ReadFiles.Data[i].Length == 0)
          return SessionResult_Invalid("compaction file paths cannot be empty");
      for (u32 i = 0; i < record->CompactionEntry.ModifiedFiles.Count; i++)
        if (record->CompactionEntry.ModifiedFiles.Data[i].Length == 0)
          return SessionResult_Invalid("compaction file paths cannot be empty");
      break;
    case SessionRecordKind::BranchLabel:
      if (!DurableId_IsSet(record->BranchLabelEntry.ParentId) ||
          record->BranchLabelEntry.Label.Length == 0)
        return SessionResult_Invalid("branch label entry is invalid");
      break;
    case SessionRecordKind::SessionName:
      break;
    case SessionRecordKind::Leaf:
      if (!DurableId_IsSet(record->LeafEntry.LeafId))
        return SessionResult_Invalid("leaf marker requires a target entry ID");
      break;
  }
  return SessionResult_Ok(record->Seq);
}

SessionResult SessionRecord_Encode(StrBuf* out, const SessionRecord* record) {
  if (!out) return SessionResult_Invalid("output buffer is null");
  SessionResult valid = SessionRecord_Validate(record);
  if (!SessionResult_IsOk(valid)) return valid;

  JsonWriter w;
  JsonWrite_Begin(&w, out);
  JsonWrite_U64(&w, "schemaVersion", record->SchemaVersion);
  WriteId(&w, "sessionId", record->SessionId);
  JsonWrite_U64(&w, "seq", record->Seq);
  WriteId(&w, "entryId", record->EntryId);
  JsonWrite_String(&w, "type", SessionRecordKind_Name(record->Kind));
  JsonWrite_U64(&w, "createdAt", record->CreatedAt);
  JsonWrite_Key(&w, "payload");
  WritePayload(out, record);
  JsonWrite_End(&w);
  return SessionResult_Ok(record->Seq);
}

SessionResult SessionRecord_Decode(Arena* arena, StrView text, SessionRecord* out) {
  if (!arena || !out || !text.Data) return SessionResult_Invalid("decode arguments are invalid");
  JsonValue* root = Json_Parse(arena, text);
  if (!Json_IsObject(root)) return SessionResult_Corrupt("record is not valid JSON object");

  SessionRecord record = {};
  u64 version = 0;
  if (!ReadRequiredU64(root, "schemaVersion", &version) || version > 0xFFFFFFFFull)
    return SessionResult_Corrupt("schemaVersion is missing or invalid");
  record.SchemaVersion = (u32)version;
  if (record.SchemaVersion != SESSION_SCHEMA_VERSION) {
    return SessionResult_Error(
        record.SchemaVersion > SESSION_SCHEMA_VERSION ? SessionStatus::UnsupportedVersion
                                                      : SessionStatus::Corrupt,
        SVL("unsupported session schema version"));
  }
  if (!ReadRequiredId(root, "sessionId", &record.SessionId) ||
      !ReadRequiredU64(root, "seq", &record.Seq) ||
      !ReadRequiredId(root, "entryId", &record.EntryId) ||
      !ParseRecordKind(Json_GetStr(root, "type"), &record.Kind) ||
      !ReadRequiredU64(root, "createdAt", &record.CreatedAt) ||
      !DecodePayload(arena, Json_Get(root, "payload"), &record)) {
    return SessionResult_Corrupt("record fields are missing or invalid");
  }

  SessionResult valid = SessionRecord_Validate(&record);
  if (!SessionResult_IsOk(valid)) {
    valid.Status = valid.Status == SessionStatus::UnsupportedVersion
                       ? SessionStatus::UnsupportedVersion
                       : SessionStatus::Corrupt;
    return valid;
  }
  *out = record;
  return SessionResult_Ok(record.Seq);
}

Message Message_Clone(Arena* arena, const Message& source) { return CloneMessage(arena, source); }

SessionResult SessionRecord_Clone(Arena* arena, const SessionRecord* source,
                                  SessionRecord* out) {
  if (!arena || !source || !out) return SessionResult_Invalid("clone arguments are invalid");
  SessionRecord clone = *source;
  switch (source->Kind) {
    case SessionRecordKind::SessionStarted:
      clone.SessionStarted.WorkingDirectory =
          StrView_Dup(arena, source->SessionStarted.WorkingDirectory);
      clone.SessionStarted.ProjectKey =
          StrView_Dup(arena, source->SessionStarted.ProjectKey);
      clone.SessionStarted.Label = StrView_Dup(arena, source->SessionStarted.Label);
      break;
    case SessionRecordKind::Message:
      clone.MessageEntry.Value = CloneMessage(arena, source->MessageEntry.Value);
      break;
    case SessionRecordKind::Model:
      clone.ModelEntry.Provider = StrView_Dup(arena, source->ModelEntry.Provider);
      clone.ModelEntry.Model = StrView_Dup(arena, source->ModelEntry.Model);
      break;
    case SessionRecordKind::Thinking:
      clone.ThinkingEntry.Level = StrView_Dup(arena, source->ThinkingEntry.Level);
      break;
    case SessionRecordKind::Tools: {
      const Array<StrView>& names = source->ToolsEntry.ToolNames;
      clone.ToolsEntry.ToolNames = CloneStringArray(arena, names);
      break;
    }
    case SessionRecordKind::Compaction:
      clone.CompactionEntry.Summary =
          CloneMessage(arena, source->CompactionEntry.Summary);
      clone.CompactionEntry.ReadFiles =
          CloneStringArray(arena, source->CompactionEntry.ReadFiles);
      clone.CompactionEntry.ModifiedFiles =
          CloneStringArray(arena, source->CompactionEntry.ModifiedFiles);
      break;
    case SessionRecordKind::BranchLabel:
      clone.BranchLabelEntry.Label = StrView_Dup(arena, source->BranchLabelEntry.Label);
      break;
    case SessionRecordKind::SessionName:
      clone.SessionNameEntry.Name = StrView_Dup(arena, source->SessionNameEntry.Name);
      break;
    case SessionRecordKind::Leaf:
      break;
  }
  *out = clone;
  return SessionResult_Ok(source->Seq);
}
