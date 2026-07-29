#include "session/entry.h"

#include <string.h>

#include "json/json.h"

namespace {

constexpr f64 JSON_SAFE_INTEGER_MAX = 9007199254740991.0;

struct ObjectWriter {
  StrBuf* Out;
  bool First;
};

void ObjectBegin(ObjectWriter* w, StrBuf* out) {
  w->Out = out;
  w->First = true;
  StrBuf_AppendChar(out, '{');
}

void ObjectKey(ObjectWriter* w, const char* key) {
  if (!w->First) StrBuf_AppendChar(w->Out, ',');
  w->First = false;
  Json_WriteString(w->Out, SV(key));
  StrBuf_AppendChar(w->Out, ':');
}

void ObjectEnd(ObjectWriter* w) { StrBuf_AppendChar(w->Out, '}'); }

void WriteString(ObjectWriter* w, const char* key, StrView value) {
  ObjectKey(w, key);
  Json_WriteString(w->Out, value);
}

void WriteBool(ObjectWriter* w, const char* key, bool value) {
  ObjectKey(w, key);
  StrBuf_AppendCStr(w->Out, value ? "true" : "false");
}

void WriteU64(ObjectWriter* w, const char* key, u64 value) {
  ObjectKey(w, key);
  StrBuf_AppendU64(w->Out, value);
}

void WriteI64(ObjectWriter* w, const char* key, i64 value) {
  ObjectKey(w, key);
  StrBuf_AppendI64(w->Out, value);
}

void WriteId(ObjectWriter* w, const char* key, DurableId id) {
  ObjectKey(w, key);
  Json_WriteString(w->Out, DurableId_View(&id));
}

void WriteOptionalId(ObjectWriter* w, const char* key, DurableId id) {
  ObjectKey(w, key);
  if (DurableId_IsSet(id))
    Json_WriteString(w->Out, DurableId_View(&id));
  else
    StrBuf_AppendCStr(w->Out, "null");
}

StrView ClassName(SessionEntryClass value) {
  switch (value) {
    case SessionEntryClass::Session: return SVL("session");
    case SessionEntryClass::Harness: return SVL("harness");
  }
  return StrView{};
}

bool ParseClass(StrView text, SessionEntryClass* out) {
  if (StrView_Eq(text, SVL("session"))) {
    *out = SessionEntryClass::Session;
    return true;
  }
  if (StrView_Eq(text, SVL("harness"))) {
    *out = SessionEntryClass::Harness;
    return true;
  }
  return false;
}

StrView OperationKindName(OperationKind value) {
  switch (value) {
    case OperationKind::Run: return SVL("run");
    case OperationKind::Compaction: return SVL("compaction");
    case OperationKind::Navigation: return SVL("navigation");
  }
  return StrView{};
}

bool ParseOperationKind(StrView text, OperationKind* out) {
  if (StrView_Eq(text, SVL("run"))) {
    *out = OperationKind::Run;
    return true;
  }
  if (StrView_Eq(text, SVL("compaction"))) {
    *out = OperationKind::Compaction;
    return true;
  }
  if (StrView_Eq(text, SVL("navigation"))) {
    *out = OperationKind::Navigation;
    return true;
  }
  return false;
}

StrView GenerationOutcomeName(GenerationOutcome value) {
  switch (value) {
    case GenerationOutcome::Succeeded: return SVL("succeeded");
    case GenerationOutcome::Failed: return SVL("failed");
    case GenerationOutcome::Aborted: return SVL("aborted");
  }
  return StrView{};
}

bool ParseGenerationOutcome(StrView text, GenerationOutcome* out) {
  if (StrView_Eq(text, SVL("succeeded"))) {
    *out = GenerationOutcome::Succeeded;
    return true;
  }
  if (StrView_Eq(text, SVL("failed"))) {
    *out = GenerationOutcome::Failed;
    return true;
  }
  if (StrView_Eq(text, SVL("aborted"))) {
    *out = GenerationOutcome::Aborted;
    return true;
  }
  return false;
}

StrView ToolOutcomeName(ToolOutcomeKind value) {
  switch (value) {
    case ToolOutcomeKind::Succeeded: return SVL("succeeded");
    case ToolOutcomeKind::Failed: return SVL("failed");
    case ToolOutcomeKind::Aborted: return SVL("aborted");
  }
  return StrView{};
}

bool ParseToolOutcome(StrView text, ToolOutcomeKind* out) {
  if (StrView_Eq(text, SVL("succeeded"))) {
    *out = ToolOutcomeKind::Succeeded;
    return true;
  }
  if (StrView_Eq(text, SVL("failed"))) {
    *out = ToolOutcomeKind::Failed;
    return true;
  }
  if (StrView_Eq(text, SVL("aborted"))) {
    *out = ToolOutcomeKind::Aborted;
    return true;
  }
  return false;
}

StrView OperationOutcomeName(OperationOutcome value) {
  switch (value) {
    case OperationOutcome::Succeeded: return SVL("succeeded");
    case OperationOutcome::Failed: return SVL("failed");
    case OperationOutcome::Cancelled: return SVL("cancelled");
  }
  return StrView{};
}

bool ParseOperationOutcome(StrView text, OperationOutcome* out) {
  if (StrView_Eq(text, SVL("succeeded"))) {
    *out = OperationOutcome::Succeeded;
    return true;
  }
  if (StrView_Eq(text, SVL("failed"))) {
    *out = OperationOutcome::Failed;
    return true;
  }
  if (StrView_Eq(text, SVL("cancelled"))) {
    *out = OperationOutcome::Cancelled;
    return true;
  }
  return false;
}

StrView QueueKindName(QueueKind value) {
  switch (value) {
    case QueueKind::Steering: return SVL("steering");
    case QueueKind::FollowUp: return SVL("follow_up");
    case QueueKind::NextTurn: return SVL("next_turn");
  }
  return StrView{};
}

bool ParseQueueKind(StrView text, QueueKind* out) {
  if (StrView_Eq(text, SVL("steering"))) {
    *out = QueueKind::Steering;
    return true;
  }
  if (StrView_Eq(text, SVL("follow_up"))) {
    *out = QueueKind::FollowUp;
    return true;
  }
  if (StrView_Eq(text, SVL("next_turn"))) {
    *out = QueueKind::NextTurn;
    return true;
  }
  return false;
}

StrView CheckpointName(QueueCheckpointKind value) {
  switch (value) {
    case QueueCheckpointKind::BeforeAssistant: return SVL("before_assistant");
    case QueueCheckpointKind::AfterToolBatch: return SVL("after_tool_batch");
    case QueueCheckpointKind::AfterRun: return SVL("after_run");
  }
  return StrView{};
}

bool ParseCheckpoint(StrView text, QueueCheckpointKind* out) {
  if (StrView_Eq(text, SVL("before_assistant"))) {
    *out = QueueCheckpointKind::BeforeAssistant;
    return true;
  }
  if (StrView_Eq(text, SVL("after_tool_batch"))) {
    *out = QueueCheckpointKind::AfterToolBatch;
    return true;
  }
  if (StrView_Eq(text, SVL("after_run"))) {
    *out = QueueCheckpointKind::AfterRun;
    return true;
  }
  return false;
}

StrView ReplayPolicyName(ToolReplayPolicy value) {
  switch (value) {
    case ToolReplayPolicy::ManualReconcile: return SVL("manual_reconcile");
    case ToolReplayPolicy::ReplaySafe: return SVL("replay_safe");
    case ToolReplayPolicy::NoEffect: return SVL("no_effect");
  }
  return StrView{};
}

bool ParseReplayPolicy(StrView text, ToolReplayPolicy* out) {
  if (StrView_Eq(text, SVL("manual_reconcile"))) {
    *out = ToolReplayPolicy::ManualReconcile;
    return true;
  }
  if (StrView_Eq(text, SVL("replay_safe"))) {
    *out = ToolReplayPolicy::ReplaySafe;
    return true;
  }
  if (StrView_Eq(text, SVL("no_effect"))) {
    *out = ToolReplayPolicy::NoEffect;
    return true;
  }
  return false;
}

StrView RecoveryResolutionName(RecoveryResolution value) {
  switch (value) {
    case RecoveryResolution::RecordFailure: return SVL("record_failure");
    case RecoveryResolution::SupplyResult: return SVL("supply_result");
    case RecoveryResolution::RerunAnyway: return SVL("rerun_anyway");
  }
  return StrView{};
}

bool ParseRecoveryResolution(StrView text, RecoveryResolution* out) {
  if (StrView_Eq(text, SVL("record_failure"))) {
    *out = RecoveryResolution::RecordFailure;
    return true;
  }
  if (StrView_Eq(text, SVL("supply_result"))) {
    *out = RecoveryResolution::SupplyResult;
    return true;
  }
  if (StrView_Eq(text, SVL("rerun_anyway"))) {
    *out = RecoveryResolution::RerunAnyway;
    return true;
  }
  return false;
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
  for (u32 i = 0; i <= (u32)SessionRecordKind::RecoveryResolved; i++) {
    SessionRecordKind kind = (SessionRecordKind)i;
    if (StrView_Eq(text, SessionRecordKind_Name(kind))) {
      *out = kind;
      return true;
    }
  }
  return false;
}

SessionResult Corrupt(const char* message) {
  return SessionResult_Error(SessionStatus::Corrupt, SV(message));
}

SessionResult Invalid(const char* message) {
  return SessionResult_Error(SessionStatus::InvalidArgument, SV(message));
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

bool ReadRequiredI32(const JsonValue* object, const char* key, i32* out) {
  JsonValue* value = Json_Get(object, key);
  if (!value || value->Kind != JsonKind::Number || value->Number < -2147483648.0 ||
      value->Number > 2147483647.0) {
    return false;
  }
  i32 converted = (i32)value->Number;
  if ((f64)converted != value->Number) return false;
  *out = converted;
  return true;
}

bool ReadRequiredId(const JsonValue* object, const char* key, DurableId* out) {
  JsonValue* value = Json_Get(object, key);
  return Json_IsString(value) && DurableId_Parse(value->Str, out);
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
  ObjectWriter w;
  ObjectBegin(&w, out);
  WriteString(&w, "kind", ContentKindName(content->Kind));
  switch (content->Kind) {
    case ContentKind::Text:
      WriteString(&w, "text", content->Text);
      WriteString(&w, "signature", content->Signature);
      break;
    case ContentKind::Thinking:
      WriteString(&w, "text", content->Text);
      WriteString(&w, "signature", content->Signature);
      WriteBool(&w, "redacted", content->Redacted);
      break;
    case ContentKind::ToolCall:
      WriteString(&w, "toolId", content->ToolId);
      WriteString(&w, "toolName", content->ToolName);
      WriteString(&w, "argumentsJson", content->ToolArgsJson);
      break;
    case ContentKind::Image:
      WriteString(&w, "data", content->ImageData);
      WriteString(&w, "mimeType", content->ImageMimeType);
      break;
  }
  ObjectEnd(&w);
}

void WriteUsage(StrBuf* out, const Usage* usage) {
  ObjectWriter w;
  ObjectBegin(&w, out);
  WriteU64(&w, "input", usage->Input);
  WriteU64(&w, "output", usage->Output);
  WriteU64(&w, "cacheRead", usage->CacheRead);
  WriteU64(&w, "cacheWrite", usage->CacheWrite);
  WriteU64(&w, "reasoning", usage->Reasoning);
  WriteU64(&w, "totalTokens", usage->TotalTokens);
  ObjectEnd(&w);
}

void WriteMessage(StrBuf* out, const Message* message) {
  ObjectWriter w;
  ObjectBegin(&w, out);
  WriteString(&w, "role", RoleName(message->Kind));
  ObjectKey(&w, "content");
  StrBuf_AppendChar(out, '[');
  for (u32 i = 0; i < message->Content.Count; i++) {
    if (i) StrBuf_AppendChar(out, ',');
    WriteContent(out, &message->Content.Data[i]);
  }
  StrBuf_AppendChar(out, ']');
  WriteU64(&w, "timestamp", message->Timestamp);
  if (message->Kind == Role::Assistant) {
    WriteString(&w, "stopReason", StopReasonName(message->Stop));
    ObjectKey(&w, "usage");
    WriteUsage(out, &message->Use);
    WriteString(&w, "errorMessage", message->ErrorMessage);
    WriteString(&w, "model", message->Model);
    WriteString(&w, "provider", message->Provider);
  } else if (message->Kind == Role::ToolResult) {
    WriteString(&w, "toolCallId", message->ToolCallId);
    WriteString(&w, "toolName", message->ToolName);
    WriteBool(&w, "isError", message->IsError);
  }
  ObjectEnd(&w);
}

bool DecodeContent(Arena* arena, const JsonValue* object, Content* out) {
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
          !ReadRequiredString(object, "argumentsJson", false, &result.ToolArgsJson)) {
        return false;
      }
      JsonValue* args = Json_Parse(arena, result.ToolArgsJson);
      if (!Json_IsObject(args)) return false;
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
    if (!DecodeContent(arena, content->Items[i], &item)) return false;
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

void WriteIdArray(StrBuf* out, const Array<DurableId>* ids) {
  StrBuf_AppendChar(out, '[');
  for (u32 i = 0; i < ids->Count; i++) {
    if (i) StrBuf_AppendChar(out, ',');
    Json_WriteString(out, DurableId_View(&ids->Data[i]));
  }
  StrBuf_AppendChar(out, ']');
}

bool DecodeIdArray(Arena* arena, const JsonValue* value, Array<DurableId>* out) {
  if (!Json_IsArray(value)) return false;
  Array<DurableId> ids = Array_Make<DurableId>(arena, value->Count ? value->Count : 1);
  for (u32 i = 0; i < value->Count; i++) {
    DurableId id = {};
    if (!Json_IsString(value->Items[i]) || !DurableId_Parse(value->Items[i]->Str, &id))
      return false;
    Array_Push(&ids, id);
  }
  *out = ids;
  return true;
}

void WritePayload(StrBuf* out, const SessionRecord* record) {
  ObjectWriter w;
  ObjectBegin(&w, out);
  switch (record->Kind) {
    case SessionRecordKind::SessionStarted:
      WriteString(&w, "workingDirectory", record->SessionStarted.WorkingDirectory);
      WriteString(&w, "projectKey", record->SessionStarted.ProjectKey);
      WriteString(&w, "label", record->SessionStarted.Label);
      WriteOptionalId(&w, "forkedFromSessionId", record->SessionStarted.ForkedFromSessionId);
      WriteOptionalId(&w, "forkedFromEntryId", record->SessionStarted.ForkedFromEntryId);
      break;
    case SessionRecordKind::Message:
      WriteId(&w, "parentId", record->MessageEntry.ParentId);
      ObjectKey(&w, "message");
      WriteMessage(out, &record->MessageEntry.Value);
      break;
    case SessionRecordKind::Model:
      WriteId(&w, "parentId", record->ModelEntry.ParentId);
      WriteString(&w, "provider", record->ModelEntry.Provider);
      WriteString(&w, "model", record->ModelEntry.Model);
      break;
    case SessionRecordKind::Thinking:
      WriteId(&w, "parentId", record->ThinkingEntry.ParentId);
      WriteString(&w, "level", record->ThinkingEntry.Level);
      break;
    case SessionRecordKind::Tools:
      WriteId(&w, "parentId", record->ToolsEntry.ParentId);
      ObjectKey(&w, "toolNames");
      StrBuf_AppendChar(out, '[');
      for (u32 i = 0; i < record->ToolsEntry.ToolNames.Count; i++) {
        if (i) StrBuf_AppendChar(out, ',');
        Json_WriteString(out, record->ToolsEntry.ToolNames.Data[i]);
      }
      StrBuf_AppendChar(out, ']');
      break;
    case SessionRecordKind::Compaction:
      WriteId(&w, "parentId", record->CompactionEntry.ParentId);
      WriteId(&w, "baseEntryId", record->CompactionEntry.BaseEntryId);
      WriteId(&w, "throughEntryId", record->CompactionEntry.ThroughEntryId);
      WriteId(&w, "sourceHeadId", record->CompactionEntry.SourceHeadId);
      ObjectKey(&w, "summary");
      WriteMessage(out, &record->CompactionEntry.Summary);
      break;
    case SessionRecordKind::BranchLabel:
      WriteId(&w, "parentId", record->BranchLabelEntry.ParentId);
      WriteString(&w, "label", record->BranchLabelEntry.Label);
      break;
    case SessionRecordKind::SessionName:
      WriteString(&w, "name", record->SessionNameEntry.Name);
      break;
    case SessionRecordKind::OperationStarted:
      WriteId(&w, "operationId", record->OperationStarted.OperationId);
      WriteString(&w, "operation", OperationKindName(record->OperationStarted.Operation));
      WriteOptionalId(&w, "startingHeadId", record->OperationStarted.StartingHeadId);
      WriteBool(&w, "hasAcceptedMessage", record->OperationStarted.HasAcceptedMessage);
      if (record->OperationStarted.HasAcceptedMessage) {
        ObjectKey(&w, "acceptedMessage");
        WriteMessage(out, &record->OperationStarted.AcceptedMessage);
      }
      WriteOptionalId(&w, "provisionedEntryId", record->OperationStarted.ProvisionedEntryId);
      WriteOptionalId(&w, "targetEntryId", record->OperationStarted.TargetEntryId);
      break;
    case SessionRecordKind::GenerationStarted:
      WriteId(&w, "operationId", record->GenerationStarted.OperationId);
      WriteId(&w, "attemptId", record->GenerationStarted.AttemptId);
      WriteString(&w, "provider", record->GenerationStarted.Provider);
      WriteString(&w, "model", record->GenerationStarted.Model);
      WriteString(&w, "effectiveRequestJson",
                  record->GenerationStarted.EffectiveRequestJson);
      WriteI64(&w, "maxTokens", record->GenerationStarted.MaxTokens);
      WriteId(&w, "provisionedAssistantEntryId",
              record->GenerationStarted.ProvisionedAssistantEntryId);
      break;
    case SessionRecordKind::GenerationFinished:
      WriteId(&w, "operationId", record->GenerationFinished.OperationId);
      WriteId(&w, "attemptId", record->GenerationFinished.AttemptId);
      WriteString(&w, "outcome", GenerationOutcomeName(record->GenerationFinished.Outcome));
      WriteOptionalId(&w, "messageEntryId", record->GenerationFinished.MessageEntryId);
      WriteString(&w, "error", record->GenerationFinished.Error);
      if (record->GenerationFinished.HasUsage) {
        ObjectKey(&w, "usage");
        WriteUsage(out, &record->GenerationFinished.Use);
      }
      break;
    case SessionRecordKind::ToolStarted:
      WriteId(&w, "operationId", record->ToolStarted.OperationId);
      WriteId(&w, "toolExecutionId", record->ToolStarted.ToolExecutionId);
      WriteString(&w, "toolCallId", record->ToolStarted.ToolCallId);
      WriteString(&w, "toolName", record->ToolStarted.ToolName);
      WriteString(&w, "effectiveArgumentsJson",
                  record->ToolStarted.EffectiveArgumentsJson);
      WriteId(&w, "provisionedResultEntryId",
              record->ToolStarted.ProvisionedResultEntryId);
      WriteString(&w, "replayPolicy", ReplayPolicyName(record->ToolStarted.ReplayPolicy));
      break;
    case SessionRecordKind::ToolFinished:
      WriteId(&w, "operationId", record->ToolFinished.OperationId);
      WriteId(&w, "toolExecutionId", record->ToolFinished.ToolExecutionId);
      WriteString(&w, "outcome", ToolOutcomeName(record->ToolFinished.Outcome));
      WriteOptionalId(&w, "resultEntryId", record->ToolFinished.ResultEntryId);
      WriteBool(&w, "terminate", record->ToolFinished.Terminate);
      WriteString(&w, "error", record->ToolFinished.Error);
      break;
    case SessionRecordKind::QueueItemAccepted:
      WriteOptionalId(&w, "operationId", record->QueueItemAccepted.OperationId);
      WriteId(&w, "itemId", record->QueueItemAccepted.ItemId);
      WriteString(&w, "queue", QueueKindName(record->QueueItemAccepted.Queue));
      ObjectKey(&w, "message");
      WriteMessage(out, &record->QueueItemAccepted.Value);
      break;
    case SessionRecordKind::QueueCheckpoint:
      WriteId(&w, "operationId", record->QueueCheckpoint.OperationId);
      WriteString(&w, "checkpoint", CheckpointName(record->QueueCheckpoint.Checkpoint));
      ObjectKey(&w, "itemIds");
      WriteIdArray(out, &record->QueueCheckpoint.ItemIds);
      break;
    case SessionRecordKind::DeferredWrite:
      WriteId(&w, "operationId", record->DeferredWrite.OperationId);
      WriteId(&w, "deferredId", record->DeferredWrite.DeferredId);
      WriteId(&w, "queueItemId", record->DeferredWrite.QueueItemId);
      WriteId(&w, "provisionedEntryId", record->DeferredWrite.ProvisionedEntryId);
      ObjectKey(&w, "message");
      WriteMessage(out, &record->DeferredWrite.Value);
      break;
    case SessionRecordKind::DeferredWriteCommitted:
      WriteId(&w, "operationId", record->DeferredWriteCommitted.OperationId);
      WriteId(&w, "deferredId", record->DeferredWriteCommitted.DeferredId);
      WriteId(&w, "messageEntryId", record->DeferredWriteCommitted.MessageEntryId);
      break;
    case SessionRecordKind::OperationCancelRequested:
      WriteId(&w, "operationId", record->OperationCancelRequested.OperationId);
      break;
    case SessionRecordKind::OperationFinished:
      WriteId(&w, "operationId", record->OperationFinished.OperationId);
      WriteString(&w, "outcome", OperationOutcomeName(record->OperationFinished.Outcome));
      WriteOptionalId(&w, "resultHeadId", record->OperationFinished.ResultHeadId);
      WriteString(&w, "error", record->OperationFinished.Error);
      break;
    case SessionRecordKind::OperationAborted:
      WriteId(&w, "operationId", record->OperationAborted.OperationId);
      WriteString(&w, "reason", record->OperationAborted.Reason);
      break;
    case SessionRecordKind::RecoveryResolved:
      WriteId(&w, "operationId", record->RecoveryResolved.OperationId);
      WriteId(&w, "toolExecutionId", record->RecoveryResolved.ToolExecutionId);
      WriteString(&w, "resolution",
                  RecoveryResolutionName(record->RecoveryResolved.Resolution));
      WriteBool(&w, "hasSuppliedResult", record->RecoveryResolved.HasSuppliedResult);
      if (record->RecoveryResolved.HasSuppliedResult) {
        ObjectKey(&w, "suppliedResult");
        WriteMessage(out, &record->RecoveryResolved.SuppliedResult);
      }
      break;
  }
  ObjectEnd(&w);
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
    case SessionRecordKind::Compaction:
      return ReadRequiredId(payload, "parentId", &record->CompactionEntry.ParentId) &&
             ReadRequiredId(payload, "baseEntryId",
                            &record->CompactionEntry.BaseEntryId) &&
             ReadRequiredId(payload, "throughEntryId",
                            &record->CompactionEntry.ThroughEntryId) &&
             ReadRequiredId(payload, "sourceHeadId",
                            &record->CompactionEntry.SourceHeadId) &&
             DecodeMessage(arena, Json_Get(payload, "summary"),
                           &record->CompactionEntry.Summary);
    case SessionRecordKind::BranchLabel:
      return ReadRequiredId(payload, "parentId", &record->BranchLabelEntry.ParentId) &&
             ReadRequiredString(payload, "label", false, &record->BranchLabelEntry.Label);
    case SessionRecordKind::SessionName:
      return ReadRequiredString(payload, "name", true,
                                &record->SessionNameEntry.Name);
    case SessionRecordKind::OperationStarted: {
      if (!ReadRequiredId(payload, "operationId",
                          &record->OperationStarted.OperationId) ||
          !ParseOperationKind(Json_GetStr(payload, "operation"),
                              &record->OperationStarted.Operation) ||
          !ReadOptionalId(payload, "startingHeadId",
                          &record->OperationStarted.StartingHeadId) ||
          !ReadRequiredBool(payload, "hasAcceptedMessage",
                            &record->OperationStarted.HasAcceptedMessage) ||
          !ReadOptionalId(payload, "provisionedEntryId",
                          &record->OperationStarted.ProvisionedEntryId) ||
          !ReadOptionalId(payload, "targetEntryId",
                          &record->OperationStarted.TargetEntryId)) {
        return false;
      }
      return !record->OperationStarted.HasAcceptedMessage ||
             DecodeMessage(arena, Json_Get(payload, "acceptedMessage"),
                           &record->OperationStarted.AcceptedMessage);
    }
    case SessionRecordKind::GenerationStarted: {
      if (!ReadRequiredId(payload, "operationId",
                          &record->GenerationStarted.OperationId) ||
          !ReadRequiredId(payload, "attemptId", &record->GenerationStarted.AttemptId) ||
          !ReadRequiredString(payload, "provider", false,
                              &record->GenerationStarted.Provider) ||
          !ReadRequiredString(payload, "model", false, &record->GenerationStarted.Model) ||
          !ReadRequiredString(payload, "effectiveRequestJson", false,
                              &record->GenerationStarted.EffectiveRequestJson) ||
          !ReadRequiredI32(payload, "maxTokens", &record->GenerationStarted.MaxTokens) ||
          !ReadRequiredId(payload, "provisionedAssistantEntryId",
                          &record->GenerationStarted.ProvisionedAssistantEntryId)) {
        return false;
      }
      return Json_IsObject(
          Json_Parse(arena, record->GenerationStarted.EffectiveRequestJson));
    }
    case SessionRecordKind::GenerationFinished: {
      if (!ReadRequiredId(payload, "operationId",
                          &record->GenerationFinished.OperationId) ||
          !ReadRequiredId(payload, "attemptId",
                          &record->GenerationFinished.AttemptId) ||
          !ParseGenerationOutcome(Json_GetStr(payload, "outcome"),
                                  &record->GenerationFinished.Outcome) ||
          !ReadOptionalId(payload, "messageEntryId",
                          &record->GenerationFinished.MessageEntryId) ||
          !ReadRequiredString(payload, "error", true,
                              &record->GenerationFinished.Error)) {
        return false;
      }
      JsonValue* usage = Json_Get(payload, "usage");
      if (!usage) return true;
      record->GenerationFinished.HasUsage = true;
      return DecodeUsage(usage, &record->GenerationFinished.Use);
    }
    case SessionRecordKind::ToolStarted: {
      if (!ReadRequiredId(payload, "operationId", &record->ToolStarted.OperationId) ||
          !ReadRequiredId(payload, "toolExecutionId",
                          &record->ToolStarted.ToolExecutionId) ||
          !ReadRequiredString(payload, "toolCallId", false,
                              &record->ToolStarted.ToolCallId) ||
          !ReadRequiredString(payload, "toolName", false, &record->ToolStarted.ToolName) ||
          !ReadRequiredString(payload, "effectiveArgumentsJson", false,
                              &record->ToolStarted.EffectiveArgumentsJson) ||
          !ReadRequiredId(payload, "provisionedResultEntryId",
                          &record->ToolStarted.ProvisionedResultEntryId) ||
          !ParseReplayPolicy(Json_GetStr(payload, "replayPolicy"),
                             &record->ToolStarted.ReplayPolicy)) {
        return false;
      }
      return Json_IsObject(Json_Parse(arena, record->ToolStarted.EffectiveArgumentsJson));
    }
    case SessionRecordKind::ToolFinished:
      return ReadRequiredId(payload, "operationId", &record->ToolFinished.OperationId) &&
             ReadRequiredId(payload, "toolExecutionId",
                            &record->ToolFinished.ToolExecutionId) &&
             ParseToolOutcome(Json_GetStr(payload, "outcome"),
                              &record->ToolFinished.Outcome) &&
             ReadOptionalId(payload, "resultEntryId",
                            &record->ToolFinished.ResultEntryId) &&
             ReadRequiredBool(payload, "terminate",
                              &record->ToolFinished.Terminate) &&
             ReadRequiredString(payload, "error", true, &record->ToolFinished.Error);
    case SessionRecordKind::QueueItemAccepted:
      return ReadOptionalId(payload, "operationId",
                            &record->QueueItemAccepted.OperationId) &&
             ReadRequiredId(payload, "itemId", &record->QueueItemAccepted.ItemId) &&
             ParseQueueKind(Json_GetStr(payload, "queue"),
                            &record->QueueItemAccepted.Queue) &&
             DecodeMessage(arena, Json_Get(payload, "message"),
                           &record->QueueItemAccepted.Value);
    case SessionRecordKind::QueueCheckpoint:
      return ReadRequiredId(payload, "operationId",
                            &record->QueueCheckpoint.OperationId) &&
             ParseCheckpoint(Json_GetStr(payload, "checkpoint"),
                             &record->QueueCheckpoint.Checkpoint) &&
             DecodeIdArray(arena, Json_Get(payload, "itemIds"),
                           &record->QueueCheckpoint.ItemIds);
    case SessionRecordKind::DeferredWrite:
      return ReadRequiredId(payload, "operationId", &record->DeferredWrite.OperationId) &&
             ReadRequiredId(payload, "deferredId", &record->DeferredWrite.DeferredId) &&
             ReadRequiredId(payload, "queueItemId", &record->DeferredWrite.QueueItemId) &&
             ReadRequiredId(payload, "provisionedEntryId",
                            &record->DeferredWrite.ProvisionedEntryId) &&
             DecodeMessage(arena, Json_Get(payload, "message"),
                           &record->DeferredWrite.Value);
    case SessionRecordKind::DeferredWriteCommitted:
      return ReadRequiredId(payload, "operationId",
                            &record->DeferredWriteCommitted.OperationId) &&
             ReadRequiredId(payload, "deferredId",
                            &record->DeferredWriteCommitted.DeferredId) &&
             ReadRequiredId(payload, "messageEntryId",
                            &record->DeferredWriteCommitted.MessageEntryId);
    case SessionRecordKind::OperationCancelRequested:
      return ReadRequiredId(payload, "operationId",
                            &record->OperationCancelRequested.OperationId);
    case SessionRecordKind::OperationFinished:
      return ReadRequiredId(payload, "operationId",
                            &record->OperationFinished.OperationId) &&
             ParseOperationOutcome(Json_GetStr(payload, "outcome"),
                                   &record->OperationFinished.Outcome) &&
             ReadOptionalId(payload, "resultHeadId",
                            &record->OperationFinished.ResultHeadId) &&
             ReadRequiredString(payload, "error", true, &record->OperationFinished.Error);
    case SessionRecordKind::OperationAborted:
      return ReadRequiredId(payload, "operationId",
                            &record->OperationAborted.OperationId) &&
             ReadRequiredString(payload, "reason", false,
                                &record->OperationAborted.Reason);
    case SessionRecordKind::RecoveryResolved: {
      if (!ReadRequiredId(payload, "operationId",
                          &record->RecoveryResolved.OperationId) ||
          !ReadRequiredId(payload, "toolExecutionId",
                          &record->RecoveryResolved.ToolExecutionId) ||
          !ParseRecoveryResolution(Json_GetStr(payload, "resolution"),
                                   &record->RecoveryResolved.Resolution) ||
          !ReadRequiredBool(payload, "hasSuppliedResult",
                            &record->RecoveryResolved.HasSuppliedResult)) {
        return false;
      }
      return !record->RecoveryResolved.HasSuppliedResult ||
             DecodeMessage(arena, Json_Get(payload, "suppliedResult"),
                           &record->RecoveryResolved.SuppliedResult);
    }
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

DurableId OperationIdFor(const SessionRecord* record) {
  switch (record->Kind) {
    case SessionRecordKind::OperationStarted: return record->OperationStarted.OperationId;
    case SessionRecordKind::GenerationStarted: return record->GenerationStarted.OperationId;
    case SessionRecordKind::GenerationFinished: return record->GenerationFinished.OperationId;
    case SessionRecordKind::ToolStarted: return record->ToolStarted.OperationId;
    case SessionRecordKind::ToolFinished: return record->ToolFinished.OperationId;
    case SessionRecordKind::QueueItemAccepted: return record->QueueItemAccepted.OperationId;
    case SessionRecordKind::QueueCheckpoint: return record->QueueCheckpoint.OperationId;
    case SessionRecordKind::DeferredWrite: return record->DeferredWrite.OperationId;
    case SessionRecordKind::DeferredWriteCommitted:
      return record->DeferredWriteCommitted.OperationId;
    case SessionRecordKind::OperationCancelRequested:
      return record->OperationCancelRequested.OperationId;
    case SessionRecordKind::OperationFinished: return record->OperationFinished.OperationId;
    case SessionRecordKind::OperationAborted: return record->OperationAborted.OperationId;
    case SessionRecordKind::RecoveryResolved: return record->RecoveryResolved.OperationId;
    default: return DurableId{};
  }
}

const SessionRecord* FindEarlierEntry(const SessionRecord* records, u32 before,
                                      DurableId entryId) {
  for (u32 i = 0; i < before; i++) {
    if (DurableId_Eq(records[i].EntryId, entryId)) return &records[i];
  }
  return nullptr;
}

const SessionRecord* FindEarlierKindId(const SessionRecord* records, u32 before,
                                       SessionRecordKind kind, DurableId id) {
  for (u32 i = 0; i < before; i++) {
    if (records[i].Kind != kind) continue;
    DurableId candidate = {};
    switch (kind) {
      case SessionRecordKind::GenerationStarted:
        candidate = records[i].GenerationStarted.AttemptId;
        break;
      case SessionRecordKind::ToolStarted:
        candidate = records[i].ToolStarted.ToolExecutionId;
        break;
      case SessionRecordKind::QueueItemAccepted:
        candidate = records[i].QueueItemAccepted.ItemId;
        break;
      case SessionRecordKind::DeferredWrite:
        candidate = records[i].DeferredWrite.DeferredId;
        break;
      default: break;
    }
    if (DurableId_Eq(candidate, id)) return &records[i];
  }
  return nullptr;
}

const SessionRecord* FindEarlierOperation(const SessionRecord* records, u32 before,
                                          DurableId operationId) {
  for (u32 i = 0; i < before; i++) {
    if (records[i].Kind == SessionRecordKind::OperationStarted &&
        DurableId_Eq(records[i].OperationStarted.OperationId, operationId))
      return &records[i];
  }
  return nullptr;
}

bool IsEarlierSessionEntry(const SessionRecord* records, u32 before, DurableId id) {
  const SessionRecord* found = FindEarlierEntry(records, before, id);
  return found && found->EntryClass == SessionEntryClass::Session;
}

SessionResult ValidateReference(const SessionRecord* records, u32 before, DurableId id,
                                const char* message) {
  if (!DurableId_IsSet(id) || !IsEarlierSessionEntry(records, before, id))
    return Corrupt(message);
  return SessionResult_Ok();
}

}  // namespace

SessionEntryClass SessionRecord_ClassForKind(SessionRecordKind kind) {
  return kind <= SessionRecordKind::SessionName ? SessionEntryClass::Session
                                                : SessionEntryClass::Harness;
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
    case SessionRecordKind::OperationStarted: return SVL("operation_started");
    case SessionRecordKind::GenerationStarted: return SVL("generation_started");
    case SessionRecordKind::GenerationFinished: return SVL("generation_finished");
    case SessionRecordKind::ToolStarted: return SVL("tool_started");
    case SessionRecordKind::ToolFinished: return SVL("tool_finished");
    case SessionRecordKind::QueueItemAccepted: return SVL("queue_item_accepted");
    case SessionRecordKind::QueueCheckpoint: return SVL("queue_checkpoint");
    case SessionRecordKind::DeferredWrite: return SVL("deferred_write");
    case SessionRecordKind::DeferredWriteCommitted: return SVL("deferred_write_committed");
    case SessionRecordKind::OperationCancelRequested:
      return SVL("operation_cancel_requested");
    case SessionRecordKind::OperationFinished: return SVL("operation_finished");
    case SessionRecordKind::OperationAborted: return SVL("operation_aborted");
    case SessionRecordKind::RecoveryResolved: return SVL("recovery_resolved");
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
    return Invalid("fork copy arguments are invalid");

  SessionRecord copy = *source;
  copy.SchemaVersion = SESSION_SCHEMA_VERSION;
  copy.SessionId = targetSessionId;
  copy.Seq = targetSeq;
  copy.EntryId = DurableId_Generate();
  copy.EntryClass = SessionEntryClass::Session;
  copy.CreatedAt = createdAt;

  DurableId parent =
      RemapId(oldIds, newIds, SessionRecord_ParentId(source));
  if (!DurableId_IsSet(parent))
    return Corrupt("fork branch parent could not be remapped");

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
        return Corrupt("fork compaction could not be remapped");
      break;
    default:
      return Corrupt("fork branch contains a harness record");
  }

  *out = copy;
  return SessionResult_Ok(targetSeq);
}

SessionResult SessionRecord_Validate(const SessionRecord* record) {
  if (!record) return Invalid("record is null");
  if (record->SchemaVersion != SESSION_SCHEMA_VERSION) {
    return SessionResult_Error(
        record->SchemaVersion > SESSION_SCHEMA_VERSION ? SessionStatus::UnsupportedVersion
                                                       : SessionStatus::Corrupt,
        SVL("unsupported session schema version"));
  }
  if (!DurableId_IsSet(record->SessionId)) return Invalid("sessionId is required");
  if (!DurableId_IsSet(record->EntryId)) return Invalid("entryId is required");
  if (record->Seq == 0 || record->Seq > (u64)JSON_SAFE_INTEGER_MAX)
    return Invalid("seq must be a positive JSON-safe integer");
  if (record->CreatedAt > (u64)JSON_SAFE_INTEGER_MAX)
    return Invalid("createdAt must be a JSON-safe integer");
  if (SessionRecordKind_Name(record->Kind).Length == 0)
    return Invalid("record kind is invalid");
  if (record->EntryClass != SessionRecord_ClassForKind(record->Kind))
    return Invalid("entryClass does not match record kind");

  switch (record->Kind) {
    case SessionRecordKind::SessionStarted:
      if (record->SessionStarted.WorkingDirectory.Length == 0 ||
          record->SessionStarted.ProjectKey.Length == 0)
        return Invalid("session metadata is incomplete");
      if (DurableId_IsSet(record->SessionStarted.ForkedFromSessionId) !=
          DurableId_IsSet(record->SessionStarted.ForkedFromEntryId))
        return Invalid("fork provenance must include both session and entry IDs");
      break;
    case SessionRecordKind::Message:
      if (!DurableId_IsSet(record->MessageEntry.ParentId) ||
          !MessageValid(&record->MessageEntry.Value))
        return Invalid("message entry is invalid");
      break;
    case SessionRecordKind::Model:
      if (!DurableId_IsSet(record->ModelEntry.ParentId) ||
          record->ModelEntry.Provider.Length == 0 || record->ModelEntry.Model.Length == 0)
        return Invalid("model entry is invalid");
      break;
    case SessionRecordKind::Thinking:
      if (!DurableId_IsSet(record->ThinkingEntry.ParentId) ||
          record->ThinkingEntry.Level.Length == 0)
        return Invalid("thinking entry is invalid");
      break;
    case SessionRecordKind::Tools:
      if (!DurableId_IsSet(record->ToolsEntry.ParentId))
        return Invalid("tools parentId is required");
      for (u32 i = 0; i < record->ToolsEntry.ToolNames.Count; i++)
        if (record->ToolsEntry.ToolNames.Data[i].Length == 0)
          return Invalid("tool names cannot be empty");
      break;
    case SessionRecordKind::Compaction:
      if (!DurableId_IsSet(record->CompactionEntry.ParentId) ||
          !DurableId_IsSet(record->CompactionEntry.BaseEntryId) ||
          !DurableId_IsSet(record->CompactionEntry.ThroughEntryId) ||
          !DurableId_IsSet(record->CompactionEntry.SourceHeadId) ||
          !MessageValid(&record->CompactionEntry.Summary))
        return Invalid("compaction entry is invalid");
      break;
    case SessionRecordKind::BranchLabel:
      if (!DurableId_IsSet(record->BranchLabelEntry.ParentId) ||
          record->BranchLabelEntry.Label.Length == 0)
        return Invalid("branch label entry is invalid");
      break;
    case SessionRecordKind::SessionName:
      break;
    case SessionRecordKind::OperationStarted:
      if (!DurableId_IsSet(record->OperationStarted.OperationId) ||
          OperationKindName(record->OperationStarted.Operation).Length == 0)
        return Invalid("operation start is invalid");
      if (record->OperationStarted.HasAcceptedMessage &&
          (!MessageValid(&record->OperationStarted.AcceptedMessage) ||
           !DurableId_IsSet(record->OperationStarted.ProvisionedEntryId)))
        return Invalid("accepted message requires a provisioned entry ID");
      if (record->OperationStarted.Operation == OperationKind::Run &&
          !record->OperationStarted.HasAcceptedMessage)
        return Invalid("run requires an accepted message");
      if (record->OperationStarted.Operation == OperationKind::Compaction &&
          !DurableId_IsSet(record->OperationStarted.ProvisionedEntryId))
        return Invalid("compaction requires a provisioned entry ID");
      if (record->OperationStarted.Operation == OperationKind::Navigation &&
          !DurableId_IsSet(record->OperationStarted.TargetEntryId))
        return Invalid("navigation requires a target entry ID");
      break;
    case SessionRecordKind::GenerationStarted:
      if (!DurableId_IsSet(record->GenerationStarted.OperationId) ||
          !DurableId_IsSet(record->GenerationStarted.AttemptId) ||
          record->GenerationStarted.Provider.Length == 0 ||
          record->GenerationStarted.Model.Length == 0 ||
          record->GenerationStarted.EffectiveRequestJson.Length == 0 ||
          record->GenerationStarted.MaxTokens < 0 ||
          !DurableId_IsSet(record->GenerationStarted.ProvisionedAssistantEntryId))
        return Invalid("generation start is invalid");
      break;
    case SessionRecordKind::GenerationFinished:
      if (!DurableId_IsSet(record->GenerationFinished.OperationId) ||
          !DurableId_IsSet(record->GenerationFinished.AttemptId) ||
          GenerationOutcomeName(record->GenerationFinished.Outcome).Length == 0)
        return Invalid("generation result is invalid");
      if (record->GenerationFinished.Outcome == GenerationOutcome::Succeeded &&
          !DurableId_IsSet(record->GenerationFinished.MessageEntryId))
        return Invalid("successful generation requires a message entry");
      break;
    case SessionRecordKind::ToolStarted:
      if (!DurableId_IsSet(record->ToolStarted.OperationId) ||
          !DurableId_IsSet(record->ToolStarted.ToolExecutionId) ||
          record->ToolStarted.ToolCallId.Length == 0 ||
          record->ToolStarted.ToolName.Length == 0 ||
          record->ToolStarted.EffectiveArgumentsJson.Length == 0 ||
          !DurableId_IsSet(record->ToolStarted.ProvisionedResultEntryId) ||
          ReplayPolicyName(record->ToolStarted.ReplayPolicy).Length == 0)
        return Invalid("tool start is invalid");
      break;
    case SessionRecordKind::ToolFinished:
      if (!DurableId_IsSet(record->ToolFinished.OperationId) ||
          !DurableId_IsSet(record->ToolFinished.ToolExecutionId) ||
          ToolOutcomeName(record->ToolFinished.Outcome).Length == 0)
        return Invalid("tool result is invalid");
      if (record->ToolFinished.Outcome != ToolOutcomeKind::Aborted &&
          !DurableId_IsSet(record->ToolFinished.ResultEntryId))
        return Invalid("completed tool requires a result entry");
      break;
    case SessionRecordKind::QueueItemAccepted:
      if (!DurableId_IsSet(record->QueueItemAccepted.ItemId) ||
          QueueKindName(record->QueueItemAccepted.Queue).Length == 0 ||
          !MessageValid(&record->QueueItemAccepted.Value))
        return Invalid("queue item is invalid");
      break;
    case SessionRecordKind::QueueCheckpoint:
      if (!DurableId_IsSet(record->QueueCheckpoint.OperationId) ||
          CheckpointName(record->QueueCheckpoint.Checkpoint).Length == 0)
        return Invalid("queue checkpoint is invalid");
      for (u32 i = 0; i < record->QueueCheckpoint.ItemIds.Count; i++)
        if (!DurableId_IsSet(record->QueueCheckpoint.ItemIds.Data[i]))
          return Invalid("queue checkpoint item ID is invalid");
      break;
    case SessionRecordKind::DeferredWrite:
      if (!DurableId_IsSet(record->DeferredWrite.OperationId) ||
          !DurableId_IsSet(record->DeferredWrite.DeferredId) ||
          !DurableId_IsSet(record->DeferredWrite.QueueItemId) ||
          !DurableId_IsSet(record->DeferredWrite.ProvisionedEntryId) ||
          !MessageValid(&record->DeferredWrite.Value))
        return Invalid("deferred write is invalid");
      break;
    case SessionRecordKind::DeferredWriteCommitted:
      if (!DurableId_IsSet(record->DeferredWriteCommitted.OperationId) ||
          !DurableId_IsSet(record->DeferredWriteCommitted.DeferredId) ||
          !DurableId_IsSet(record->DeferredWriteCommitted.MessageEntryId))
        return Invalid("deferred write commit is invalid");
      break;
    case SessionRecordKind::OperationCancelRequested:
      if (!DurableId_IsSet(record->OperationCancelRequested.OperationId))
        return Invalid("cancel request operationId is required");
      break;
    case SessionRecordKind::OperationFinished:
      if (!DurableId_IsSet(record->OperationFinished.OperationId) ||
          OperationOutcomeName(record->OperationFinished.Outcome).Length == 0)
        return Invalid("operation result is invalid");
      break;
    case SessionRecordKind::OperationAborted:
      if (!DurableId_IsSet(record->OperationAborted.OperationId) ||
          record->OperationAborted.Reason.Length == 0)
        return Invalid("operation abort is invalid");
      break;
    case SessionRecordKind::RecoveryResolved:
      if (!DurableId_IsSet(record->RecoveryResolved.OperationId) ||
          !DurableId_IsSet(record->RecoveryResolved.ToolExecutionId) ||
          RecoveryResolutionName(record->RecoveryResolved.Resolution).Length == 0)
        return Invalid("recovery resolution is invalid");
      if (record->RecoveryResolved.HasSuppliedResult !=
          (record->RecoveryResolved.Resolution == RecoveryResolution::SupplyResult))
        return Invalid("only supply_result may include a supplied result");
      if (record->RecoveryResolved.HasSuppliedResult &&
          !MessageValid(&record->RecoveryResolved.SuppliedResult))
        return Invalid("supplied recovery result is invalid");
      break;
  }
  return SessionResult_Ok(record->Seq);
}

SessionResult SessionRecord_Encode(StrBuf* out, const SessionRecord* record) {
  if (!out) return Invalid("output buffer is null");
  SessionResult valid = SessionRecord_Validate(record);
  if (!SessionResult_IsOk(valid)) return valid;

  ObjectWriter w;
  ObjectBegin(&w, out);
  WriteU64(&w, "schemaVersion", record->SchemaVersion);
  WriteId(&w, "sessionId", record->SessionId);
  WriteU64(&w, "seq", record->Seq);
  WriteId(&w, "entryId", record->EntryId);
  WriteString(&w, "entryClass", ClassName(record->EntryClass));
  WriteString(&w, "type", SessionRecordKind_Name(record->Kind));
  WriteU64(&w, "createdAt", record->CreatedAt);
  ObjectKey(&w, "payload");
  WritePayload(out, record);
  ObjectEnd(&w);
  return SessionResult_Ok(record->Seq);
}

SessionResult SessionRecord_Decode(Arena* arena, StrView text, SessionRecord* out) {
  if (!arena || !out || !text.Data) return Invalid("decode arguments are invalid");
  JsonValue* root = Json_Parse(arena, text);
  if (!Json_IsObject(root)) return Corrupt("record is not valid JSON object");

  SessionRecord record = {};
  u64 version = 0;
  if (!ReadRequiredU64(root, "schemaVersion", &version) || version > 0xFFFFFFFFull)
    return Corrupt("schemaVersion is missing or invalid");
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
      !ParseClass(Json_GetStr(root, "entryClass"), &record.EntryClass) ||
      !ParseRecordKind(Json_GetStr(root, "type"), &record.Kind) ||
      !ReadRequiredU64(root, "createdAt", &record.CreatedAt) ||
      !DecodePayload(arena, Json_Get(root, "payload"), &record)) {
    return Corrupt("record fields are missing or invalid");
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

SessionResult SessionRecord_Clone(Arena* arena, const SessionRecord* source,
                                  SessionRecord* out) {
  if (!arena || !source || !out) return Invalid("clone arguments are invalid");
  StrBuf encoded = StrBuf_Make(arena, 512);
  SessionResult write = SessionRecord_Encode(&encoded, source);
  if (!SessionResult_IsOk(write)) return write;
  return SessionRecord_Decode(arena, StrBuf_View(&encoded), out);
}

SessionResult SessionRecords_Validate(const SessionRecord* records, u32 count) {
  if (!records || count == 0) return Corrupt("session log is empty");
  DurableId sessionId = records[0].SessionId;
  DurableId activeOperation = {};

  for (u32 i = 0; i < count; i++) {
    const SessionRecord* record = &records[i];
    SessionResult valid = SessionRecord_Validate(record);
    if (!SessionResult_IsOk(valid)) {
      valid.Status = valid.Status == SessionStatus::UnsupportedVersion
                         ? SessionStatus::UnsupportedVersion
                         : SessionStatus::Corrupt;
      valid.Seq = record->Seq;
      return valid;
    }
    if (!DurableId_Eq(record->SessionId, sessionId))
      return SessionResult_Error(SessionStatus::Corrupt, SVL("sessionId changed within log"),
                                 record->Seq);
    if (record->Seq != (u64)i + 1)
      return SessionResult_Error(SessionStatus::Corrupt, SVL("sequence is not contiguous"),
                                 record->Seq);
    for (u32 j = 0; j < i; j++) {
      if (DurableId_Eq(record->EntryId, records[j].EntryId))
        return SessionResult_Error(SessionStatus::Corrupt, SVL("duplicate entryId"),
                                   record->Seq);
    }
    if (i == 0) {
      if (record->Kind != SessionRecordKind::SessionStarted)
        return SessionResult_Error(SessionStatus::Corrupt,
                                   SVL("first record must start the session"), record->Seq);
    } else if (record->Kind == SessionRecordKind::SessionStarted) {
      return SessionResult_Error(SessionStatus::Corrupt, SVL("duplicate session start"),
                                 record->Seq);
    }

    if (record->EntryClass == SessionEntryClass::Session &&
        record->Kind != SessionRecordKind::SessionStarted &&
        record->Kind != SessionRecordKind::SessionName) {
      SessionResult parent =
          ValidateReference(records, i, ParentIdFor(record), "session parent is invalid");
      if (!SessionResult_IsOk(parent)) {
        parent.Seq = record->Seq;
        return parent;
      }
    }

    if (record->Kind == SessionRecordKind::Compaction) {
      DurableId refs[] = {record->CompactionEntry.BaseEntryId,
                          record->CompactionEntry.ThroughEntryId,
                          record->CompactionEntry.SourceHeadId};
      for (u32 r = 0; r < 3; r++) {
        SessionResult ref =
            ValidateReference(records, i, refs[r], "compaction reference is invalid");
        if (!SessionResult_IsOk(ref)) {
          ref.Seq = record->Seq;
          return ref;
        }
      }
    }

    if (record->Kind == SessionRecordKind::OperationStarted) {
      if (DurableId_IsSet(activeOperation))
        return SessionResult_Error(SessionStatus::Corrupt,
                                   SVL("multiple operations are active"), record->Seq);
      for (u32 j = 0; j < i; j++) {
        if (records[j].Kind == SessionRecordKind::OperationStarted &&
            DurableId_Eq(records[j].OperationStarted.OperationId,
                         record->OperationStarted.OperationId)) {
          return SessionResult_Error(SessionStatus::Corrupt, SVL("duplicate operationId"),
                                     record->Seq);
        }
      }
      if (DurableId_IsSet(record->OperationStarted.StartingHeadId) &&
          !IsEarlierSessionEntry(records, i, record->OperationStarted.StartingHeadId))
        return SessionResult_Error(SessionStatus::Corrupt,
                                   SVL("operation starting head is invalid"), record->Seq);
      if (DurableId_IsSet(record->OperationStarted.TargetEntryId) &&
          !IsEarlierSessionEntry(records, i, record->OperationStarted.TargetEntryId))
        return SessionResult_Error(SessionStatus::Corrupt,
                                   SVL("operation target is invalid"), record->Seq);
      activeOperation = record->OperationStarted.OperationId;
      continue;
    }

    DurableId operationId = OperationIdFor(record);
    if (DurableId_IsSet(operationId)) {
      bool idleQueueItem =
          record->Kind == SessionRecordKind::QueueItemAccepted &&
          !DurableId_IsSet(record->QueueItemAccepted.OperationId);
      if (!idleQueueItem &&
          (!DurableId_IsSet(activeOperation) ||
           !DurableId_Eq(operationId, activeOperation))) {
        return SessionResult_Error(SessionStatus::Corrupt,
                                   SVL("record does not belong to the active operation"),
                                   record->Seq);
      }
    }

    if (record->Kind == SessionRecordKind::GenerationStarted) {
      if (FindEarlierKindId(records, i, SessionRecordKind::GenerationStarted,
                            record->GenerationStarted.AttemptId))
        return SessionResult_Error(SessionStatus::Corrupt, SVL("duplicate attemptId"),
                                   record->Seq);
    } else if (record->Kind == SessionRecordKind::GenerationFinished) {
      const SessionRecord* started =
          FindEarlierKindId(records, i, SessionRecordKind::GenerationStarted,
                            record->GenerationFinished.AttemptId);
      if (!started ||
          !DurableId_Eq(started->GenerationStarted.OperationId,
                        record->GenerationFinished.OperationId))
        return SessionResult_Error(SessionStatus::Corrupt,
                                   SVL("generation result has no matching attempt"),
                                   record->Seq);
      if (record->GenerationFinished.Outcome == GenerationOutcome::Succeeded) {
        const SessionRecord* result =
            FindEarlierEntry(records, i, record->GenerationFinished.MessageEntryId);
        const SessionRecord* operation =
            FindEarlierOperation(records, i, record->GenerationFinished.OperationId);
        bool validMessage =
            result && result->Kind == SessionRecordKind::Message &&
            result->MessageEntry.Value.Kind == Role::Assistant;
        bool validCompaction =
            result && result->Kind == SessionRecordKind::Compaction && operation &&
            operation->OperationStarted.Operation == OperationKind::Compaction;
        if ((!validMessage && !validCompaction) ||
            !DurableId_Eq(record->GenerationFinished.MessageEntryId,
                          started->GenerationStarted.ProvisionedAssistantEntryId))
          return SessionResult_Error(SessionStatus::Corrupt,
                                     SVL("generation result entry is invalid"), record->Seq);
      }
    } else if (record->Kind == SessionRecordKind::ToolStarted) {
      if (FindEarlierKindId(records, i, SessionRecordKind::ToolStarted,
                            record->ToolStarted.ToolExecutionId))
        return SessionResult_Error(SessionStatus::Corrupt,
                                   SVL("duplicate toolExecutionId"), record->Seq);
    } else if (record->Kind == SessionRecordKind::ToolFinished) {
      const SessionRecord* started =
          FindEarlierKindId(records, i, SessionRecordKind::ToolStarted,
                            record->ToolFinished.ToolExecutionId);
      if (!started ||
          !DurableId_Eq(started->ToolStarted.OperationId,
                        record->ToolFinished.OperationId))
        return SessionResult_Error(SessionStatus::Corrupt,
                                   SVL("tool result has no matching start"), record->Seq);
      if (record->ToolFinished.Outcome != ToolOutcomeKind::Aborted) {
        const SessionRecord* result =
            FindEarlierEntry(records, i, record->ToolFinished.ResultEntryId);
        if (!result || result->Kind != SessionRecordKind::Message ||
            result->MessageEntry.Value.Kind != Role::ToolResult ||
            !DurableId_Eq(record->ToolFinished.ResultEntryId,
                          started->ToolStarted.ProvisionedResultEntryId))
          return SessionResult_Error(SessionStatus::Corrupt,
                                     SVL("tool result message is invalid"), record->Seq);
      }
    } else if (record->Kind == SessionRecordKind::QueueCheckpoint) {
      for (u32 item = 0; item < record->QueueCheckpoint.ItemIds.Count; item++) {
        if (!FindEarlierKindId(records, i, SessionRecordKind::QueueItemAccepted,
                               record->QueueCheckpoint.ItemIds.Data[item]))
          return SessionResult_Error(SessionStatus::Corrupt,
                                     SVL("checkpoint queue item is invalid"), record->Seq);
      }
    } else if (record->Kind == SessionRecordKind::DeferredWrite) {
      if (!FindEarlierKindId(records, i, SessionRecordKind::QueueItemAccepted,
                             record->DeferredWrite.QueueItemId))
        return SessionResult_Error(SessionStatus::Corrupt,
                                   SVL("deferred write queue item is invalid"), record->Seq);
      if (FindEarlierKindId(records, i, SessionRecordKind::DeferredWrite,
                            record->DeferredWrite.DeferredId))
        return SessionResult_Error(SessionStatus::Corrupt, SVL("duplicate deferredId"),
                                   record->Seq);
    } else if (record->Kind == SessionRecordKind::DeferredWriteCommitted) {
      const SessionRecord* deferred =
          FindEarlierKindId(records, i, SessionRecordKind::DeferredWrite,
                            record->DeferredWriteCommitted.DeferredId);
      const SessionRecord* message =
          FindEarlierEntry(records, i, record->DeferredWriteCommitted.MessageEntryId);
      if (!deferred || !message || message->Kind != SessionRecordKind::Message ||
          !DurableId_Eq(deferred->DeferredWrite.ProvisionedEntryId,
                        record->DeferredWriteCommitted.MessageEntryId))
        return SessionResult_Error(SessionStatus::Corrupt,
                                   SVL("deferred write commit is invalid"), record->Seq);
    } else if (record->Kind == SessionRecordKind::RecoveryResolved) {
      if (!FindEarlierKindId(records, i, SessionRecordKind::ToolStarted,
                             record->RecoveryResolved.ToolExecutionId))
        return SessionResult_Error(SessionStatus::Corrupt,
                                   SVL("recovery tool reference is invalid"), record->Seq);
    } else if (record->Kind == SessionRecordKind::OperationFinished ||
               record->Kind == SessionRecordKind::OperationAborted) {
      if (record->Kind == SessionRecordKind::OperationFinished &&
          DurableId_IsSet(record->OperationFinished.ResultHeadId) &&
          !IsEarlierSessionEntry(records, i, record->OperationFinished.ResultHeadId))
        return SessionResult_Error(SessionStatus::Corrupt,
                                   SVL("operation result head is invalid"), record->Seq);
      activeOperation = DurableId{};
    }
  }

  return SessionResult_Ok(records[count - 1].Seq);
}
