#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "json/json.h"
#include "llm/responses.h"

namespace {

bool Eq(StrView value, const char* expected) {
  return StrView_EqCStr(value, expected);
}

void WriteCanonicalObject(StrBuf* out, StrView json, StrView fallback) {
  if (!json.Length) {
    StrBuf_Append(out, fallback);
    return;
  }
  u32 reserve =
      json.Length <= (UINT_MAX - 64) / 2 ? json.Length * 2 + 64 : json.Length;
  StrBuf_Ensure(out, reserve);
  usize mark = out->Backing->Used;
  JsonValue* value = Json_Parse(out->Backing, json);
  if (Json_IsObject(value))
    Json_WriteValue(out, value);
  else
    StrBuf_Append(out, fallback);
  ArenaRestore(out->Backing, mark);
}

// ---- request ------------------------------------------------------------

void WriteUserContent(StrBuf* out, const Message* message) {
  bool first = true;
  for (u32 i = 0; i < message->Content.Count; i++) {
    const Content* content = &message->Content.Data[i];
    if (content->Kind != ContentKind::Text &&
        content->Kind != ContentKind::Image)
      continue;
    if (!first) StrBuf_AppendChar(out, ',');
    first = false;
    if (content->Kind == ContentKind::Text) {
      StrBuf_AppendCStr(out, "{\"type\":\"input_text\",\"text\":");
      Json_WriteString(out, content->Text);
      StrBuf_AppendChar(out, '}');
      continue;
    }
    StrBuf_AppendCStr(
        out, "{\"type\":\"input_image\",\"detail\":\"auto\",\"image_url\":");
    StrBuf dataUrl = StrBuf_Make(
        out->Backing,
        content->ImageMimeType.Length + content->ImageData.Length + 16);
    StrBuf_AppendCStr(&dataUrl, "data:");
    StrBuf_Append(&dataUrl, content->ImageMimeType);
    StrBuf_AppendCStr(&dataUrl, ";base64,");
    StrBuf_Append(&dataUrl, content->ImageData);
    Json_WriteString(out, StrBuf_View(&dataUrl));
    StrBuf_AppendChar(out, '}');
  }
}

void WriteToolOutput(StrBuf* out, const Message* message) {
  StrBuf text = StrBuf_Make(out->Backing, 128);
  for (u32 i = 0; i < message->Content.Count; i++) {
    const Content* content = &message->Content.Data[i];
    if (content->Kind != ContentKind::Text) continue;
    if (text.Length) StrBuf_AppendChar(&text, '\n');
    StrBuf_Append(&text, content->Text);
  }
  Json_WriteString(out, StrBuf_View(&text));
}

// An assistant turn only belongs in the request when it carries something the
// server can act on. Errored and aborted turns never do, and a turn that
// produced only unreplayable reasoning contributes nothing.
bool AssistantIsSendable(const Message* message) {
  if (!message || message->Stop == StopReason::Error ||
      message->Stop == StopReason::Aborted)
    return false;
  for (u32 i = 0; i < message->Content.Count; i++) {
    const Content* content = &message->Content.Data[i];
    if (content->Kind == ContentKind::Text ||
        content->Kind == ContentKind::ToolCall)
      return true;
    if (content->Kind == ContentKind::Thinking && content->Signature.Length)
      return true;
  }
  return false;
}

// Item identities are scoped to the model that issued them; replaying one
// against a different model is rejected. A turn recorded by another model is
// still sent, just under generated ids and without its reasoning.
bool SignaturesReplayable(const Message* message, const Model* model) {
  return message && model && StrView_Eq(message->Provider, model->Provider) &&
         StrView_Eq(message->Model, model->Id);
}

void WriteTextItem(StrBuf* out, Arena* arena, const Content* content,
                   u32 messageIndex, u32 blockIndex, bool replayable) {
  StrView id = {};
  StrView phase = {};
  if (replayable && content->Signature.Length) {
    JsonValue* signature = Json_Parse(arena, content->Signature);
    if (Json_IsObject(signature)) {
      id = Json_GetStr(signature, "id");
      phase = Json_GetStr(signature, "phase");
    }
  }
  StrBuf fallbackId = {};
  if (!id.Length || id.Length > 64) {
    fallbackId = StrBuf_Make(out->Backing, 48);
    StrBuf_AppendCStr(&fallbackId, "msg_clanker_");
    StrBuf_AppendU64(&fallbackId, messageIndex);
    StrBuf_AppendChar(&fallbackId, '_');
    StrBuf_AppendU64(&fallbackId, blockIndex);
    id = StrBuf_View(&fallbackId);
  }

  StrBuf_AppendCStr(out,
                    "{\"type\":\"message\",\"role\":\"assistant\","
                    "\"status\":\"completed\",\"id\":");
  Json_WriteString(out, id);
  if (phase.Length) {
    StrBuf_AppendCStr(out, ",\"phase\":");
    Json_WriteString(out, phase);
  }
  StrBuf_AppendCStr(out, ",\"content\":[{\"type\":\"output_text\",\"text\":");
  Json_WriteString(out, content->Text);
  StrBuf_AppendCStr(out, ",\"annotations\":[]}]}");
}

void WriteAssistantItems(StrBuf* out, Arena* arena, const Model* model,
                         const Message* message, u32 messageIndex,
                         bool* first) {
  bool replayable = SignaturesReplayable(message, model);
  for (u32 i = 0; i < message->Content.Count; i++) {
    const Content* content = &message->Content.Data[i];
    if (content->Kind == ContentKind::Thinking) {
      // The stored signature is the server's own reasoning item, encrypted
      // content and all. There is nothing to reconstruct it from, so a block
      // without one is dropped rather than approximated.
      if (!replayable || !content->Signature.Length) continue;
      u32 reserve = content->Signature.Length <= (UINT_MAX - 64) / 2
                        ? content->Signature.Length * 2 + 64
                        : content->Signature.Length;
      StrBuf_Ensure(out, reserve);
      usize mark = arena->Used;
      JsonValue* item = Json_Parse(arena, content->Signature);
      if (Json_IsObject(item) &&
          Eq(Json_GetStr(item, "type"), "reasoning")) {
        if (!*first) StrBuf_AppendChar(out, ',');
        *first = false;
        Json_WriteValue(out, item);
      }
      ArenaRestore(arena, mark);
      continue;
    }
    if (content->Kind == ContentKind::Text) {
      if (!*first) StrBuf_AppendChar(out, ',');
      *first = false;
      WriteTextItem(out, arena, content, messageIndex, i, replayable);
      continue;
    }
    if (content->Kind != ContentKind::ToolCall) continue;
    if (!*first) StrBuf_AppendChar(out, ',');
    *first = false;
    StrBuf_AppendCStr(out, "{\"type\":\"function_call\"");
    if (replayable && content->Signature.Length) {
      StrBuf_AppendCStr(out, ",\"id\":");
      Json_WriteString(out, content->Signature);
    }
    StrBuf_AppendCStr(out, ",\"call_id\":");
    Json_WriteString(out, content->ToolId);
    StrBuf_AppendCStr(out, ",\"name\":");
    Json_WriteString(out, content->ToolName);
    StrBuf_AppendCStr(out, ",\"arguments\":");
    // `arguments` is a JSON *string* holding JSON, so the object is
    // canonicalized first and then written as a string.
    StrBuf canonical =
        StrBuf_Make(out->Backing, content->ToolArgsJson.Length + 16);
    WriteCanonicalObject(&canonical, content->ToolArgsJson, SVL("{}"));
    Json_WriteString(out, StrBuf_View(&canonical));
    StrBuf_AppendChar(out, '}');
  }
}

// ---- streaming ----------------------------------------------------------

void Emit(ResponsesParse* state, StreamEventKind kind, u32 contentIndex,
          StrView delta = {}) {
  StreamEvent event = {};
  event.Kind = kind;
  event.ContentIndex = contentIndex;
  event.Delta = delta;
  StreamSink_Emit(state->Sink, event);
}

ResponsesSlot* FindSlot(ResponsesParse* state, i64 outputIndex) {
  for (u32 i = 0; i < state->Slots.Count; i++) {
    if (state->Slots.Data[i].OutputIndex == outputIndex)
      return &state->Slots.Data[i];
  }
  return nullptr;
}

// Content blocks are pushed as their items open, so a block's index is fixed
// before any delta arrives and the UI can address it immediately. The block is
// rewritten in place when the item completes.
ResponsesSlot* StartSlot(ResponsesParse* state, i64 outputIndex,
                         JsonValue* item) {
  ResponsesSlot* existing = FindSlot(state, outputIndex);
  if (existing) return existing;
  if (!Json_IsObject(item)) return nullptr;

  StrView type = Json_GetStr(item, "type");
  ResponsesSlot slot = {};
  slot.OutputIndex = outputIndex;
  slot.Value = StrBuf_Make(state->Mem, 256);
  slot.ItemId = StrView_Dup(state->Mem, Json_GetStr(item, "id"));
  slot.ContentIndex = state->Msg->Content.Count;
  if (Eq(type, "message")) {
    slot.Kind = 1;
    slot.Phase = StrView_Dup(state->Mem, Json_GetStr(item, "phase"));
    Array_Push(&state->Msg->Content, Content_Text({}));
    Emit(state, StreamEventKind::TextStart, slot.ContentIndex);
  } else if (Eq(type, "reasoning")) {
    slot.Kind = 2;
    Array_Push(&state->Msg->Content, Content_Thinking({}));
    Emit(state, StreamEventKind::ThinkingStart, slot.ContentIndex);
  } else if (Eq(type, "function_call")) {
    slot.Kind = 3;
    slot.CallId = StrView_Dup(state->Mem, Json_GetStr(item, "call_id"));
    slot.Name = StrView_Dup(state->Mem, Json_GetStr(item, "name"));
    Array_Push(&state->Msg->Content,
               Content_ToolCall(slot.CallId, slot.Name, {}));
    Emit(state, StreamEventKind::ToolCallStart, slot.ContentIndex);
  } else {
    return nullptr;
  }
  return Array_Push(&state->Slots, slot);
}

void AppendItemText(StrBuf* out, JsonValue* values) {
  if (!Json_IsArray(values)) return;
  bool wrote = false;
  for (u32 i = 0; i < values->Count; i++) {
    JsonValue* part = values->Items[i];
    StrView text = Json_GetStr(part, "text");
    if (!text.Length) text = Json_GetStr(part, "refusal");
    if (!text.Length) continue;
    if (wrote) StrBuf_AppendCStr(out, "\n\n");
    StrBuf_Append(out, text);
    wrote = true;
  }
}

StrView EncodeTextSignature(ResponsesParse* state, StrView id, StrView phase) {
  if (!id.Length) return {};
  StrBuf signature = StrBuf_Make(state->Mem, id.Length + phase.Length + 32);
  StrBuf_AppendCStr(&signature, "{\"id\":");
  Json_WriteString(&signature, id);
  if (phase.Length) {
    StrBuf_AppendCStr(&signature, ",\"phase\":");
    Json_WriteString(&signature, phase);
  }
  StrBuf_AppendChar(&signature, '}');
  return StrBuf_View(&signature);
}

// Writes the finished block. `item` is the completed output item when the
// stream delivered one, and null when the stream ended first -- in which case
// whatever deltas arrived are kept.
void CloseSlot(ResponsesParse* state, ResponsesSlot* slot, JsonValue* item) {
  if (!slot || slot->Finished) return;
  slot->Finished = true;

  StrView id = Json_GetStr(item, "id");
  if (id.Length) slot->ItemId = StrView_Dup(state->Mem, id);

  if (slot->Kind == 1) {
    StrBuf finalText = StrBuf_Make(state->Mem, slot->Value.Length + 64);
    AppendItemText(&finalText, Json_Get(item, "content"));
    if (finalText.Length) slot->Value = finalText;
    StrView phase = Json_GetStr(item, "phase");
    if (phase.Length) slot->Phase = StrView_Dup(state->Mem, phase);
    Content text =
        Content_Text(StrView_Dup(state->Mem, StrBuf_View(&slot->Value)));
    text.Signature = EncodeTextSignature(state, slot->ItemId, slot->Phase);
    state->Msg->Content.Data[slot->ContentIndex] = text;
    Emit(state, StreamEventKind::TextEnd, slot->ContentIndex);
    return;
  }
  if (slot->Kind == 2) {
    StrBuf finalText = StrBuf_Make(state->Mem, slot->Value.Length + 64);
    AppendItemText(&finalText, Json_Get(item, "summary"));
    if (!finalText.Length) AppendItemText(&finalText, Json_Get(item, "content"));
    if (finalText.Length) slot->Value = finalText;
    Content thinking =
        Content_Thinking(StrView_Dup(state->Mem, StrBuf_View(&slot->Value)));
    if (Json_IsObject(item)) {
      StrBuf signature = StrBuf_Make(state->Mem, 256);
      Json_WriteValue(&signature, item);
      thinking.Signature = StrBuf_View(&signature);
    }
    state->Msg->Content.Data[slot->ContentIndex] = thinking;
    Emit(state, StreamEventKind::ThinkingEnd, slot->ContentIndex);
    return;
  }
  if (slot->Kind != 3) return;
  StrView callId = Json_GetStr(item, "call_id");
  StrView name = Json_GetStr(item, "name");
  StrView arguments = Json_GetStr(item, "arguments");
  if (callId.Length) slot->CallId = StrView_Dup(state->Mem, callId);
  if (name.Length) slot->Name = StrView_Dup(state->Mem, name);
  if (arguments.Length) {
    StrBuf_Clear(&slot->Value);
    StrBuf_Append(&slot->Value, arguments);
  }
  StrView args = slot->Value.Length
                     ? StrView_Dup(state->Mem, StrBuf_View(&slot->Value))
                     : SVL("{}");
  Content call = Content_ToolCall(slot->CallId, slot->Name, args);
  call.Signature = slot->ItemId;
  state->Msg->Content.Data[slot->ContentIndex] = call;
  state->SawTool = true;
  Emit(state, StreamEventKind::ToolCallEnd, slot->ContentIndex);
}

u32 ReadU32(JsonValue* object, const char* name) {
  i64 value = Json_GetInt(object, name, 0);
  return value > 0 && value <= 0xFFFFFFFFll ? (u32)value : 0;
}

void ReadUsage(ResponsesParse* state, JsonValue* response) {
  JsonValue* usage = Json_Get(response, "usage");
  if (!Json_IsObject(usage)) return;
  u32 input = ReadU32(usage, "input_tokens");
  u32 output = ReadU32(usage, "output_tokens");
  JsonValue* inputDetails = Json_Get(usage, "input_tokens_details");
  JsonValue* outputDetails = Json_Get(usage, "output_tokens_details");
  u32 cached = ReadU32(inputDetails, "cached_tokens");
  state->Use.CacheRead = cached;
  // `input_tokens` is inclusive of cached tokens; subtract so Input keeps
  // meaning "billed at full rate".
  state->Use.Input = input >= cached ? input - cached : input;
  state->Use.Output = output;
  state->Use.Reasoning = ReadU32(outputDetails, "reasoning_tokens");
  state->Use.TotalTokens = ReadU32(usage, "total_tokens");
  if (!state->Use.TotalTokens) state->Use.TotalTokens = input + output;
}

void SetError(ResponsesParse* state, StrView message) {
  state->SawError = true;
  state->Stop = StopReason::Error;
  state->Error = StrView_Dup(
      state->Mem,
      message.Length ? message : SVL("the provider reported an error"));
}

void HandleEvent(ResponsesParse* state, JsonValue* event) {
  StrView type = Json_GetStr(event, "type");
  i64 outputIndex = Json_GetInt(event, "output_index", -1);
  if (Eq(type, "response.output_item.added")) {
    StartSlot(state, outputIndex, Json_Get(event, "item"));
  } else if (Eq(type, "response.output_text.delta") ||
             Eq(type, "response.refusal.delta")) {
    ResponsesSlot* slot = FindSlot(state, outputIndex);
    if (!slot) return;
    StrView delta = Json_GetStr(event, "delta");
    StrBuf_Append(&slot->Value, delta);
    Emit(state, StreamEventKind::TextDelta, slot->ContentIndex, delta);
  } else if (Eq(type, "response.reasoning_summary_text.delta") ||
             Eq(type, "response.reasoning_text.delta")) {
    ResponsesSlot* slot = FindSlot(state, outputIndex);
    if (!slot) return;
    StrView delta = Json_GetStr(event, "delta");
    StrBuf_Append(&slot->Value, delta);
    Emit(state, StreamEventKind::ThinkingDelta, slot->ContentIndex, delta);
  } else if (Eq(type, "response.reasoning_summary_part.done")) {
    ResponsesSlot* slot = FindSlot(state, outputIndex);
    if (!slot || !slot->Value.Length) return;
    StrBuf_AppendCStr(&slot->Value, "\n\n");
    Emit(state, StreamEventKind::ThinkingDelta, slot->ContentIndex,
         SVL("\n\n"));
  } else if (Eq(type, "response.function_call_arguments.delta")) {
    ResponsesSlot* slot = FindSlot(state, outputIndex);
    if (!slot) return;
    StrView delta = Json_GetStr(event, "delta");
    StrBuf_Append(&slot->Value, delta);
    Emit(state, StreamEventKind::ToolCallDelta, slot->ContentIndex, delta);
  } else if (Eq(type, "response.function_call_arguments.done")) {
    ResponsesSlot* slot = FindSlot(state, outputIndex);
    StrView arguments = Json_GetStr(event, "arguments");
    if (slot && arguments.Length) {
      StrBuf_Clear(&slot->Value);
      StrBuf_Append(&slot->Value, arguments);
    }
  } else if (Eq(type, "response.output_item.done")) {
    JsonValue* item = Json_Get(event, "item");
    ResponsesSlot* slot = FindSlot(state, outputIndex);
    if (!slot) slot = StartSlot(state, outputIndex, item);
    CloseSlot(state, slot, item);
  } else if (Eq(type, "response.completed") || Eq(type, "response.incomplete")) {
    JsonValue* response = Json_Get(event, "response");
    ReadUsage(state, response);
    state->SawTerminal = true;
    if (Eq(type, "response.incomplete")) {
      StrView reason =
          Json_GetStr(Json_Get(response, "incomplete_details"), "reason");
      if (Eq(reason, "max_output_tokens"))
        state->Stop = StopReason::Length;
      else
        SetError(state,
                 reason.Length ? reason : SVL("the response was incomplete"));
    } else {
      state->Stop = state->SawTool ? StopReason::ToolUse : StopReason::Stop;
    }
  } else if (Eq(type, "response.failed")) {
    JsonValue* response = Json_Get(event, "response");
    ReadUsage(state, response);
    StrView message = Json_GetStr(Json_Get(response, "error"), "message");
    if (!message.Length)
      message = Json_GetStr(Json_Get(response, "incomplete_details"), "reason");
    state->SawTerminal = true;
    SetError(state, message);
  } else if (Eq(type, "error")) {
    StrView message = Json_GetStr(event, "message");
    if (!message.Length)
      message = Json_GetStr(Json_Get(event, "error"), "message");
    SetError(state, message);
  }
}

void HandleData(ResponsesParse* state, StrView data) {
  if (!data.Length || Eq(data, "[DONE]")) return;
  // The payload is a slice of the parser's own writable buffer, so it can be
  // null-terminated in place for Json_Parse rather than copied.
  char* end = (char*)(data.Data + data.Length);
  char saved = *end;
  *end = '\0';
  JsonValue* event = Json_Parse(state->Mem, data);
  *end = saved;
  if (Json_IsObject(event)) HandleEvent(state, event);
}

bool OnParseLine(void* user, StrView line) {
  ResponsesParse* state = (ResponsesParse*)user;
  StrView data = {};
  if (Sse_DataPayload(line, &data)) HandleData(state, data);
  return true;
}

// ---- model metadata -----------------------------------------------------

bool JsonArrayHasString(JsonValue* values, const char* expected) {
  if (!Json_IsArray(values)) return false;
  for (u32 i = 0; i < values->Count; i++) {
    JsonValue* value = values->Items[i];
    if (Json_IsString(value) && Eq(value->Str, expected)) return true;
  }
  return false;
}

bool IsAgentModel(JsonValue* value) {
  JsonValue* parameters = Json_Get(value, "supported_parameters");
  if (!JsonArrayHasString(parameters, "tools")) return false;

  JsonValue* architecture = Json_Get(value, "architecture");
  if (!Json_IsObject(architecture)) return true;
  JsonValue* inputs = Json_Get(architecture, "input_modalities");
  JsonValue* outputs = Json_Get(architecture, "output_modalities");
  return (!Json_IsArray(inputs) || JsonArrayHasString(inputs, "text")) &&
         (!Json_IsArray(outputs) || JsonArrayHasString(outputs, "text"));
}

// OpenRouter owns these facts. If the catalog does not advertise a capability
// Clanker does not infer it from the model id.
void ApplyAdvertisedCapabilities(Model* model, JsonValue* value) {
  if (!model || !Json_IsObject(value)) return;
  JsonValue* parameters = Json_Get(value, "supported_parameters");
  model->Reasoning = JsonArrayHasString(parameters, "reasoning");
  model->EffortMask = 0;
  model->DefaultEffort = ModelEffort::None;
  if (!model->Reasoning) return;

  JsonValue* reasoning = Json_Get(value, "reasoning");
  JsonValue* efforts = Json_Get(reasoning, "supported_efforts");
  if (Json_IsArray(efforts)) {
    for (u32 i = 0; i < efforts->Count; i++) {
      JsonValue* entry = efforts->Items[i];
      ModelEffort effort = ModelEffort::None;
      if (Json_IsString(entry) && ModelEffort_Parse(entry->Str, &effort))
        model->EffortMask |= ModelEffort_Bit(effort);
    }
  }

  ModelEffort defaultEffort = ModelEffort::None;
  if (ModelEffort_Parse(Json_GetStr(reasoning, "default_effort"),
                        &defaultEffort) &&
      Model_SupportsEffort(model, defaultEffort))
    model->DefaultEffort = defaultEffort;
}

}  // namespace

StrView ResponsesEndpoint_ResolveBaseUrl(Arena* arena,
                                         const ResponsesEndpoint* endpoint) {
  if (!endpoint) return {};
  StrView base = endpoint->BaseUrl;
  if (endpoint->BaseUrlEnvVar) {
    const char* override = getenv(endpoint->BaseUrlEnvVar);
    if (override && *override) base = SV(override);
  }
  while (base.Length && base.Data[base.Length - 1] == '/') base.Length--;
  return arena ? StrView_Dup(arena, base) : base;
}

StrView Responses_Url(Arena* arena, StrView baseUrl, const char* path) {
  StrView suffix = SV(path);
  StrBuf url = StrBuf_Make(arena, baseUrl.Length + suffix.Length + 8);
  StrBuf_Append(&url, baseUrl);
  bool versioned = baseUrl.Length >= 3 &&
                   memcmp(baseUrl.Data + baseUrl.Length - 3, "/v1", 3) == 0;
  StrBuf_AppendCStr(&url, versioned ? "/" : "/v1/");
  StrBuf_Append(&url, suffix);
  return StrBuf_View(&url);
}

Model Responses_Model(Arena* arena, const ResponsesEndpoint* endpoint,
                      StrView modelId) {
  Model model = {};
  model.Id = modelId;
  model.Name = modelId;
  model.Provider = endpoint ? endpoint->Id : StrView{};
  model.BaseUrl = ResponsesEndpoint_ResolveBaseUrl(arena, endpoint);
  return model;
}

StrBuf Responses_BuildRequest(Arena* arena, const Model* model,
                              const LlmContext* context,
                              const StreamOptions* options) {
  ModelRequestProfile profile = Model_ResolveRequestProfile(
      model, options ? options->Profile : ModelRequestProfile{});
  StrBuf out = StrBuf_Make(arena, 4096);
  StrBuf_AppendCStr(&out, "{\"model\":");
  Json_WriteString(&out, model->Id);
  // Clanker keeps the whole transcript itself, so server-side state is neither
  // needed nor wanted; reasoning comes back encrypted instead.
  StrBuf_AppendCStr(&out, ",\"store\":false,\"stream\":true");

  if (context->SystemPrompt.Length) {
    StrBuf_AppendCStr(&out, ",\"instructions\":");
    Json_WriteString(&out, context->SystemPrompt);
  }

  i32 maxTokens = options && options->MaxTokens > 0
                      ? options->MaxTokens
                      : (model->MaxTokens ? (i32)model->MaxTokens : 0);
  if (maxTokens > 0) {
    StrBuf_AppendCStr(&out, ",\"max_output_tokens\":");
    StrBuf_AppendU64(&out, (u64)maxTokens);
  }

  if (profile.Effort != ModelEffort::None) {
    // "ultra" is Clanker's name for the tier the wire calls "max".
    ModelEffort wireEffort =
        profile.Effort == ModelEffort::Ultra ? ModelEffort::Max : profile.Effort;
    StrBuf_AppendCStr(&out, ",\"reasoning\":{\"effort\":");
    Json_WriteString(&out, ModelEffort_Name(wireEffort));
    StrBuf_AppendCStr(&out, ",\"summary\":\"auto\"}");
  }

  StrBuf_AppendCStr(&out,
                    ",\"include\":[\"reasoning.encrypted_content\"],"
                    "\"tool_choice\":\"auto\",\"parallel_tool_calls\":true");

  if (context->ToolCount) {
    StrBuf_AppendCStr(&out, ",\"tools\":[");
    for (u32 i = 0; i < context->ToolCount; i++) {
      if (i) StrBuf_AppendChar(&out, ',');
      const ToolDef* tool = &context->Tools[i];
      StrBuf_AppendCStr(&out, "{\"type\":\"function\",\"name\":");
      Json_WriteString(&out, tool->Name);
      StrBuf_AppendCStr(&out, ",\"description\":");
      Json_WriteString(&out, tool->Description);
      StrBuf_AppendCStr(&out, ",\"parameters\":");
      WriteCanonicalObject(&out, tool->ParamsJsonSchema,
                           SVL("{\"type\":\"object\",\"properties\":{}}"));
      StrBuf_AppendCStr(&out, ",\"strict\":false}");
    }
    StrBuf_AppendChar(&out, ']');
  }

  StrBuf_AppendCStr(&out, ",\"input\":[");
  bool first = true;
  for (u32 i = 0; i < context->MessageCount; i++) {
    const Message* message = &context->Messages[i];
    if (message->Kind == Role::User) {
      if (!first) StrBuf_AppendChar(&out, ',');
      first = false;
      StrBuf_AppendCStr(
          &out, "{\"type\":\"message\",\"role\":\"user\",\"content\":[");
      WriteUserContent(&out, message);
      StrBuf_AppendCStr(&out, "]}");
    } else if (message->Kind == Role::Assistant) {
      if (AssistantIsSendable(message))
        WriteAssistantItems(&out, arena, model, message, i, &first);
    } else if (message->Kind == Role::ToolResult) {
      if (!first) StrBuf_AppendChar(&out, ',');
      first = false;
      StrBuf_AppendCStr(&out,
                        "{\"type\":\"function_call_output\",\"call_id\":");
      Json_WriteString(&out, message->ToolCallId);
      StrBuf_AppendCStr(&out, ",\"output\":");
      WriteToolOutput(&out, message);
      StrBuf_AppendChar(&out, '}');
    }
  }
  StrBuf_AppendCStr(&out, "]}");
  return out;
}

void ResponsesParse_Init(ResponsesParse* state, Arena* arena, Message* message,
                         StreamSink sink) {
  *state = {};
  state->Mem = arena;
  state->Buffer = StrBuf_Make(arena, 4096);
  state->Msg = message;
  state->Sink = sink;
  state->Slots = Array_Make<ResponsesSlot>(arena, 8);
  state->Stop = StopReason::Stop;
}

void ResponsesParse_Feed(ResponsesParse* state, StrView chunk) {
  StrBuf_Append(&state->Buffer, chunk);
  StrBuf_DrainLines(&state->Buffer, state, OnParseLine);
}

void ResponsesParse_Finish(ResponsesParse* state) {
  if (!state || state->Finished) return;
  state->Finished = true;
  if (!state->Msg) return;
  for (u32 i = 0; i < state->Slots.Count; i++)
    CloseSlot(state, &state->Slots.Data[i], nullptr);
}

bool Responses_ParseModelsResponse(Arena* arena,
                                   const ResponsesEndpoint* endpoint,
                                   StrView response, ProviderModelList* out) {
  *out = {};
  out->Status = ProviderModelListStatus::Failed;
  JsonValue* root = Json_ParseCopy(arena, response);
  if (!Json_IsObject(root)) {
    out->Error = SVL("the models response is not a JSON object");
    return false;
  }
  JsonValue* values = Json_Get(root, "data");
  if (!Json_IsArray(values)) {
    out->Error = SVL("the models response has no data array");
    return false;
  }

  out->Models =
      values->Count ? ArenaPushArray(arena, Model, values->Count) : nullptr;
  for (u32 i = 0; i < values->Count; i++) {
    JsonValue* value = values->Items[i];
    if (!Json_IsObject(value)) {
      out->Error = SVL("the models response contains a non-object model");
      return false;
    }
    StrView id = Json_GetStr(value, "id");
    if (!id.Length) {
      out->Error = SVL("the models response contains a model without an ID");
      return false;
    }
    if (!IsAgentModel(value)) continue;

    Model model = Responses_Model(arena, endpoint, StrView_Dup(arena, id));
    u32 contextWindow = (u32)Json_GetInt(value, "context_length", 0);
    if (!contextWindow)
      contextWindow = (u32)Json_GetInt(value, "context_window", 0);
    if (contextWindow) model.ContextWindow = contextWindow;
    // A max-output figure is deliberately not read from the catalog: it is the
    // model's ceiling rather than a budget for this turn, and naming a ceiling
    // makes a metered gateway reserve credit for the whole of it.
    ApplyAdvertisedCapabilities(&model, value);
    StrView name = Json_GetStr(value, "name");
    if (name.Length) model.Name = StrView_Dup(arena, name);
    out->Models[out->Count++] = model;
  }
  out->Status = ProviderModelListStatus::Success;
  return true;
}
