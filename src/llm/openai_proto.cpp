#include <limits.h>
#include <string.h>

#include "json/json.h"
#include "llm/openai.h"

namespace {

static const Model OPENAI_FALLBACK_MODELS[] = {
    {
        SVL("gpt-5.6-sol"),
        SVL("GPT-5.6-Sol"),
        SVL("openai"),
        SVL("openai-responses"),
        SVL("https://api.openai.com"),
        272000,
        0,
        true,
        MODEL_EFFORT_ALL_MASK,
        ModelEffort::Low,
    },
    {
        SVL("gpt-5.6-terra"),
        SVL("GPT-5.6-Terra"),
        SVL("openai"),
        SVL("openai-responses"),
        SVL("https://api.openai.com"),
        272000,
        0,
        true,
        MODEL_EFFORT_ALL_MASK,
        ModelEffort::Medium,
    },
    {
        SVL("gpt-5.6-luna"),
        SVL("GPT-5.6-Luna"),
        SVL("openai"),
        SVL("openai-responses"),
        SVL("https://api.openai.com"),
        272000,
        0,
        true,
        MODEL_EFFORT_THROUGH_MAX_MASK,
        ModelEffort::Medium,
    },
    {
        SVL("gpt-5.5"),
        SVL("GPT-5.5"),
        SVL("openai"),
        SVL("openai-responses"),
        SVL("https://api.openai.com"),
        272000,
        0,
        true,
        MODEL_EFFORT_STANDARD_MASK | ModelEffort_Bit(ModelEffort::XHigh),
        ModelEffort::Medium,
    },
    {
        SVL("gpt-5.2"),
        SVL("GPT-5.2"),
        SVL("openai"),
        SVL("openai-responses"),
        SVL("https://api.openai.com"),
        272000,
        0,
        true,
        MODEL_EFFORT_STANDARD_MASK | ModelEffort_Bit(ModelEffort::XHigh),
        ModelEffort::Medium,
    },
};

bool Eq(StrView value, const char* expected) {
  return StrView_EqCStr(value, expected);
}

bool StartsWith(StrView value, StrView prefix);

bool IsResponsesModelId(StrView id) {
  StrView candidate = id;
  if (candidate.Length > 3 && candidate.Data[0] == 'f' &&
      candidate.Data[1] == 't' && candidate.Data[2] == ':')
    candidate = StrView{candidate.Data + 3, candidate.Length - 3};
  bool supportedFamily =
      StartsWith(candidate, SVL("gpt-")) ||
      StartsWith(candidate, SVL("o1")) ||
      StartsWith(candidate, SVL("o3")) ||
      StartsWith(candidate, SVL("o4")) ||
      StartsWith(candidate, SVL("codex-")) ||
      StartsWith(candidate, SVL("computer-use-"));
  if (!supportedFamily) return false;
  const char* incompatible[] = {
      "audio",      "embedding", "image", "moderation",
      "realtime",   "search",    "transcribe", "tts",
  };
  for (u32 i = 0; i < sizeof(incompatible) / sizeof(incompatible[0]); i++) {
    if (StrView_Contains(candidate, incompatible[i])) return false;
  }
  return true;
}

bool StartsWith(StrView value, StrView prefix) {
  return value.Length >= prefix.Length &&
         memcmp(value.Data, prefix.Data, prefix.Length) == 0;
}

void ApplyKnownModelMetadata(Model* model) {
  for (u32 i = 0; i < OpenAI_FallbackModelCount(); i++) {
    const Model* fallback = &OPENAI_FALLBACK_MODELS[i];
    if (!StartsWith(model->Id, fallback->Id)) continue;
    model->ContextWindow = fallback->ContextWindow;
    model->MaxTokens = fallback->MaxTokens;
    model->Reasoning = fallback->Reasoning;
    model->EffortMask = fallback->EffortMask;
    model->DefaultEffort = fallback->DefaultEffort;
    if (StrView_Eq(model->Id, fallback->Id)) model->Name = fallback->Name;
    return;
  }
}

bool HasVisibleContent(const Message* message) {
  if (!message || message->Stop == StopReason::Error ||
      message->Stop == StopReason::Aborted)
    return false;
  for (u32 i = 0; i < message->Content.Count; i++) {
    const Content* content = &message->Content.Data[i];
    if (content->Kind == ContentKind::Text ||
        content->Kind == ContentKind::ToolCall)
      return true;
    if (content->Kind == ContentKind::Thinking &&
        content->Signature.Length)
      return true;
  }
  return false;
}

void WriteCanonicalObject(StrBuf* out, StrView json, StrView fallback) {
  if (!json.Length) {
    StrBuf_Append(out, fallback);
    return;
  }
  u32 reserve = json.Length <= (UINT_MAX - 64) / 2
                    ? json.Length * 2 + 64
                    : json.Length;
  StrBuf_Ensure(out, reserve);
  usize mark = out->Backing->Used;
  JsonValue* value = Json_Parse(out->Backing, json);
  if (Json_IsObject(value))
    Json_WriteValue(out, value);
  else
    StrBuf_Append(out, fallback);
  ArenaRestore(out->Backing, mark);
}

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
    } else {
      StrBuf_AppendCStr(
          out, "{\"type\":\"input_image\",\"detail\":\"auto\",\"image_url\":");
      StrBuf dataUrl = StrBuf_Make(out->Backing,
                                   content->ImageMimeType.Length +
                                       content->ImageData.Length + 16);
      StrBuf_AppendCStr(&dataUrl, "data:");
      StrBuf_Append(&dataUrl, content->ImageMimeType);
      StrBuf_AppendCStr(&dataUrl, ";base64,");
      StrBuf_Append(&dataUrl, content->ImageData);
      Json_WriteString(out, StrBuf_View(&dataUrl));
      StrBuf_AppendChar(out, '}');
    }
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

bool SignatureCompatible(const Message* message, const Model* model) {
  return message && model && StrView_EqCStr(message->Provider, "openai") &&
         StrView_Eq(message->Model, model->Id);
}

void WriteTextOutput(StrBuf* out, Arena* arena, const Content* content,
                     u32 messageIndex, u32 blockIndex, bool compatible) {
  StrView id = {};
  StrView phase = {};
  if (compatible && content->Signature.Length) {
    JsonValue* signature = Json_Parse(arena, content->Signature);
    if (Json_IsObject(signature)) {
      id = Json_GetStr(signature, "id");
      phase = Json_GetStr(signature, "phase");
    }
  }
  StrBuf fallbackId = {};
  if (!id.Length || id.Length > 64) {
    fallbackId = StrBuf_Make(out->Backing, 48);
    StrBuf_AppendCStr(&fallbackId, "msg_clank_");
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
  bool compatible = SignatureCompatible(message, model);
  for (u32 i = 0; i < message->Content.Count; i++) {
    const Content* content = &message->Content.Data[i];
    if (content->Kind == ContentKind::Thinking) {
      if (!compatible || !content->Signature.Length) continue;
      u32 reserve =
          content->Signature.Length <= (UINT_MAX - 64) / 2
              ? content->Signature.Length * 2 + 64
              : content->Signature.Length;
      StrBuf_Ensure(out, reserve);
      usize mark = arena->Used;
      JsonValue* item = Json_Parse(arena, content->Signature);
      bool valid =
          Json_IsObject(item) && Eq(Json_GetStr(item, "type"), "reasoning");
      if (valid) {
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
      WriteTextOutput(out, arena, content, messageIndex, i, compatible);
      continue;
    }
    if (content->Kind != ContentKind::ToolCall) continue;
    if (!*first) StrBuf_AppendChar(out, ',');
    *first = false;
    StrBuf_AppendCStr(out, "{\"type\":\"function_call\"");
    if (compatible && content->Signature.Length) {
      StrBuf_AppendCStr(out, ",\"id\":");
      Json_WriteString(out, content->Signature);
    }
    StrBuf_AppendCStr(out, ",\"call_id\":");
    Json_WriteString(out, content->ToolId);
    StrBuf_AppendCStr(out, ",\"name\":");
    Json_WriteString(out, content->ToolName);
    StrBuf_AppendCStr(out, ",\"arguments\":");
    StrBuf canonical = StrBuf_Make(out->Backing,
                                   content->ToolArgsJson.Length + 16);
    WriteCanonicalObject(&canonical, content->ToolArgsJson, SVL("{}"));
    Json_WriteString(out, StrBuf_View(&canonical));
    StrBuf_AppendChar(out, '}');
  }
}

void Emit(OpenAIParse* state, StreamEventKind kind, u32 contentIndex,
          StrView delta = {}) {
  StreamEvent event = {};
  event.Kind = kind;
  event.ContentIndex = contentIndex;
  event.Delta = delta;
  StreamSink_Emit(state->Sink, event);
}

OpenAIParseSlot* FindSlot(OpenAIParse* state, i64 outputIndex) {
  for (u32 i = 0; i < state->Slots.Count; i++) {
    if (state->Slots.Data[i].OutputIndex == outputIndex)
      return &state->Slots.Data[i];
  }
  return nullptr;
}

OpenAIParseSlot* StartSlot(OpenAIParse* state, i64 outputIndex,
                           JsonValue* item) {
  OpenAIParseSlot* existing = FindSlot(state, outputIndex);
  if (existing) return existing;
  if (!Json_IsObject(item)) return nullptr;

  StrView type = Json_GetStr(item, "type");
  OpenAIParseSlot slot = {};
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
    Array_Push(&state->Msg->Content, Content_Thinking({}, {}, false));
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
  Array_Push(&state->Slots, slot);
  return &state->Slots.Data[state->Slots.Count - 1];
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

StrView EncodeTextSignature(OpenAIParse* state, StrView id, StrView phase) {
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

void FinishSlot(OpenAIParse* state, i64 outputIndex, JsonValue* item) {
  OpenAIParseSlot* slot = FindSlot(state, outputIndex);
  if (!slot) slot = StartSlot(state, outputIndex, item);
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
    state->Msg->Content.Data[slot->ContentIndex] =
        Content_Text(StrView_Dup(state->Mem, StrBuf_View(&slot->Value)));
    state->Msg->Content.Data[slot->ContentIndex].Signature =
        EncodeTextSignature(state, slot->ItemId, slot->Phase);
    Emit(state, StreamEventKind::TextEnd, slot->ContentIndex);
  } else if (slot->Kind == 2) {
    StrBuf finalText = StrBuf_Make(state->Mem, slot->Value.Length + 64);
    AppendItemText(&finalText, Json_Get(item, "summary"));
    if (!finalText.Length) AppendItemText(&finalText, Json_Get(item, "content"));
    if (finalText.Length) slot->Value = finalText;
    StrBuf signature = StrBuf_Make(state->Mem, 256);
    Json_WriteValue(&signature, item);
    state->Msg->Content.Data[slot->ContentIndex] = Content_Thinking(
        StrView_Dup(state->Mem, StrBuf_View(&slot->Value)),
        StrView_Dup(state->Mem, StrBuf_View(&signature)), false);
    Emit(state, StreamEventKind::ThinkingEnd, slot->ContentIndex);
  } else if (slot->Kind == 3) {
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
}

u32 ReadU32(JsonValue* object, const char* name) {
  i64 value = Json_GetInt(object, name, 0);
  return value > 0 && value <= 0xFFFFFFFFll ? (u32)value : 0;
}

void ReadUsage(OpenAIParse* state, JsonValue* response) {
  JsonValue* usage = Json_Get(response, "usage");
  if (!Json_IsObject(usage)) return;
  u32 input = ReadU32(usage, "input_tokens");
  u32 output = ReadU32(usage, "output_tokens");
  JsonValue* inputDetails = Json_Get(usage, "input_tokens_details");
  JsonValue* outputDetails = Json_Get(usage, "output_tokens_details");
  u32 cached = ReadU32(inputDetails, "cached_tokens");
  u32 cacheWrite = ReadU32(inputDetails, "cache_write_tokens");
  state->Use.CacheRead = cached;
  state->Use.CacheWrite = cacheWrite;
  state->Use.Input =
      input >= cached + cacheWrite ? input - cached - cacheWrite : input;
  state->Use.Output = output;
  state->Use.Reasoning = ReadU32(outputDetails, "reasoning_tokens");
  state->Use.TotalTokens = ReadU32(usage, "total_tokens");
  if (!state->Use.TotalTokens)
    state->Use.TotalTokens = input + output;
}

void SetError(OpenAIParse* state, StrView message) {
  state->SawError = true;
  state->Stop = StopReason::Error;
  state->Error = StrView_Dup(
      state->Mem, message.Length ? message : SVL("OpenAI response failed"));
}

void HandleEvent(OpenAIParse* state, JsonValue* event) {
  StrView type = Json_GetStr(event, "type");
  i64 outputIndex = Json_GetInt(event, "output_index", -1);
  if (Eq(type, "response.output_item.added")) {
    StartSlot(state, outputIndex, Json_Get(event, "item"));
  } else if (Eq(type, "response.output_text.delta") ||
             Eq(type, "response.refusal.delta")) {
    OpenAIParseSlot* slot = FindSlot(state, outputIndex);
    if (!slot) return;
    StrView delta = Json_GetStr(event, "delta");
    StrBuf_Append(&slot->Value, delta);
    Emit(state, StreamEventKind::TextDelta, slot->ContentIndex, delta);
  } else if (Eq(type, "response.reasoning_summary_text.delta") ||
             Eq(type, "response.reasoning_text.delta")) {
    OpenAIParseSlot* slot = FindSlot(state, outputIndex);
    if (!slot) return;
    StrView delta = Json_GetStr(event, "delta");
    StrBuf_Append(&slot->Value, delta);
    Emit(state, StreamEventKind::ThinkingDelta, slot->ContentIndex, delta);
  } else if (Eq(type, "response.reasoning_summary_part.done")) {
    OpenAIParseSlot* slot = FindSlot(state, outputIndex);
    if (!slot || !slot->Value.Length) return;
    StrBuf_AppendCStr(&slot->Value, "\n\n");
    Emit(state, StreamEventKind::ThinkingDelta, slot->ContentIndex,
         SVL("\n\n"));
  } else if (Eq(type, "response.function_call_arguments.delta")) {
    OpenAIParseSlot* slot = FindSlot(state, outputIndex);
    if (!slot) return;
    StrView delta = Json_GetStr(event, "delta");
    StrBuf_Append(&slot->Value, delta);
    Emit(state, StreamEventKind::ToolCallDelta, slot->ContentIndex, delta);
  } else if (Eq(type, "response.function_call_arguments.done")) {
    OpenAIParseSlot* slot = FindSlot(state, outputIndex);
    StrView arguments = Json_GetStr(event, "arguments");
    if (slot && arguments.Length) {
      StrBuf_Clear(&slot->Value);
      StrBuf_Append(&slot->Value, arguments);
    }
  } else if (Eq(type, "response.output_item.done")) {
    FinishSlot(state, outputIndex, Json_Get(event, "item"));
  } else if (Eq(type, "response.completed") ||
             Eq(type, "response.incomplete")) {
    JsonValue* response = Json_Get(event, "response");
    ReadUsage(state, response);
    state->SawTerminal = true;
    if (Eq(type, "response.incomplete")) {
      StrView reason =
          Json_GetStr(Json_Get(response, "incomplete_details"), "reason");
      if (Eq(reason, "max_output_tokens"))
        state->Stop = StopReason::Length;
      else
        SetError(state, reason.Length ? reason
                                     : SVL("OpenAI response was incomplete"));
    } else {
      state->Stop =
          state->SawTool ? StopReason::ToolUse : StopReason::Stop;
    }
  } else if (Eq(type, "response.failed")) {
    JsonValue* response = Json_Get(event, "response");
    ReadUsage(state, response);
    JsonValue* error = Json_Get(response, "error");
    StrView message = Json_GetStr(error, "message");
    if (!message.Length)
      message = Json_GetStr(Json_Get(response, "incomplete_details"),
                            "reason");
    state->SawTerminal = true;
    SetError(state, message);
  } else if (Eq(type, "error")) {
    StrView message = Json_GetStr(event, "message");
    if (!message.Length)
      message = Json_GetStr(Json_Get(event, "error"), "message");
    SetError(state, message);
  }
}

void HandleData(OpenAIParse* state, StrView data) {
  if (!data.Length || StrView_EqCStr(data, "[DONE]")) return;
  char* end = (char*)(data.Data + data.Length);
  char saved = *end;
  *end = '\0';
  JsonValue* event = Json_Parse(state->Mem, data);
  *end = saved;
  if (Json_IsObject(event)) HandleEvent(state, event);
}

void HandleLine(OpenAIParse* state, StrView line) {
  if (line.Length < 5 || memcmp(line.Data, "data:", 5) != 0) return;
  StrView data = {line.Data + 5, line.Length - 5};
  while (data.Length && (data.Data[0] == ' ' || data.Data[0] == '\t')) {
    data.Data++;
    data.Length--;
  }
  HandleData(state, data);
}

bool JsonBoolOr(JsonValue* object, const char* key, bool fallback) {
  JsonValue* value = Json_Get(object, key);
  return value && value->Kind == JsonKind::Bool ? value->Bool : fallback;
}

u32 JsonU32(JsonValue* object, const char* key) {
  JsonValue* value = Json_Get(object, key);
  if (!value || value->Kind != JsonKind::Number || value->Number < 0 ||
      value->Number > 4294967295.0)
    return 0;
  u32 result = (u32)value->Number;
  return (f64)result == value->Number ? result : 0;
}

ModelEffort ParseEffortOrNone(StrView value) {
  ModelEffort effort = ModelEffort::None;
  ModelEffort_Parse(value, &effort);
  return effort;
}

}  // namespace

const Model* OpenAI_FallbackModels() { return OPENAI_FALLBACK_MODELS; }

u32 OpenAI_FallbackModelCount() {
  return (u32)(sizeof(OPENAI_FALLBACK_MODELS) /
               sizeof(OPENAI_FALLBACK_MODELS[0]));
}

Model OpenAI_Model(StrView id) {
  Model model = {};
  model.Id = id;
  model.Name = id;
  model.Provider = SVL("openai");
  model.Api = SVL("openai-responses");
  model.BaseUrl = SVL("https://api.openai.com");
  ApplyKnownModelMetadata(&model);
  return model;
}

StrBuf OpenAI_BuildRequest(Arena* arena, const Model* model,
                           const LlmContext* context,
                           const StreamOptions* options) {
  ModelRequestProfile profile = Model_ResolveRequestProfile(
      model, options ? options->Profile : ModelRequestProfile{});
  StrBuf out = StrBuf_Make(arena, 4096);
  StrBuf_AppendCStr(&out, "{\"model\":");
  Json_WriteString(&out, model->Id);
  StrBuf_AppendCStr(&out, ",\"store\":false,\"stream\":true");
  if (context->SystemPrompt.Length) {
    StrBuf_AppendCStr(&out, ",\"instructions\":");
    Json_WriteString(&out, context->SystemPrompt);
  }
  i32 maxTokens =
      options && options->MaxTokens > 0
          ? options->MaxTokens
          : (model->MaxTokens ? (i32)model->MaxTokens : 0);
  if (maxTokens > 0) {
    StrBuf_AppendCStr(&out, ",\"max_output_tokens\":");
    StrBuf_AppendU64(&out, (u64)maxTokens);
  }
  if (profile.Effort != ModelEffort::None) {
    ModelEffort wireEffort = profile.Effort == ModelEffort::Ultra
                                 ? ModelEffort::Max
                                 : profile.Effort;
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
      StrBuf_AppendCStr(&out,
                        "{\"type\":\"message\",\"role\":\"user\","
                        "\"content\":[");
      WriteUserContent(&out, message);
      StrBuf_AppendCStr(&out, "]}");
    } else if (message->Kind == Role::Assistant) {
      if (HasVisibleContent(message))
        WriteAssistantItems(&out, arena, model, message, i, &first);
    } else if (message->Kind == Role::ToolResult) {
      if (!first) StrBuf_AppendChar(&out, ',');
      first = false;
      StrBuf_AppendCStr(
          &out, "{\"type\":\"function_call_output\",\"call_id\":");
      Json_WriteString(&out, message->ToolCallId);
      StrBuf_AppendCStr(&out, ",\"output\":");
      WriteToolOutput(&out, message);
      StrBuf_AppendChar(&out, '}');
    }
  }
  StrBuf_AppendCStr(&out, "]}");
  return out;
}

void OpenAIParse_Init(OpenAIParse* state, Arena* arena, Message* message,
                      StreamSink sink) {
  *state = {};
  state->Mem = arena;
  state->Buffer = StrBuf_Make(arena, 4096);
  state->Msg = message;
  state->Sink = sink;
  state->Slots = Array_Make<OpenAIParseSlot>(arena, 8);
  state->Stop = StopReason::Stop;
}

void OpenAIParse_Feed(OpenAIParse* state, StrView chunk) {
  StrBuf_Append(&state->Buffer, chunk);
  StrBuf* buffer = &state->Buffer;
  u32 start = 0;
  for (u32 i = 0; i < buffer->Length; i++) {
    if (buffer->Data[i] != '\n') continue;
    u32 length = i - start;
    if (length && buffer->Data[start + length - 1] == '\r') length--;
    HandleLine(state, StrView{buffer->Data + start, length});
    start = i + 1;
  }
  if (start) {
    u32 remaining = buffer->Length - start;
    memmove(buffer->Data, buffer->Data + start, remaining);
    buffer->Length = remaining;
    buffer->Data[remaining] = '\0';
  }
}

bool OpenAI_ParseModelsResponse(Arena* arena, StrView response, bool rich,
                                ProviderModelList* out) {
  *out = {};
  out->Status = ProviderModelListStatus::Failed;
  StrBuf input = StrBuf_Make(arena, response.Length + 1);
  StrBuf_Append(&input, response);
  JsonValue* root = Json_Parse(arena, StrBuf_View(&input));
  if (!Json_IsObject(root)) {
    out->Error = SVL("OpenAI models response is not a JSON object");
    return false;
  }
  JsonValue* values = Json_Get(root, rich ? "models" : "data");
  if (!Json_IsArray(values)) {
    out->Error = rich ? SVL("OpenAI catalog has no models array")
                      : SVL("OpenAI models response has no data array");
    return false;
  }

  out->Models =
      values->Count ? ArenaPushArray(arena, Model, values->Count) : nullptr;
  for (u32 i = 0; i < values->Count; i++) {
    JsonValue* value = values->Items[i];
    if (!Json_IsObject(value)) {
      out->Error = SVL("OpenAI models response contains a non-object model");
      return false;
    }
    StrView id = Json_GetStr(value, rich ? "slug" : "id");
    if (!id.Length) {
      out->Error = SVL("OpenAI models response contains a model without an ID");
      return false;
    }
    if (rich) {
      StrView visibility = Json_GetStr(value, "visibility");
      if ((visibility.Length && !Eq(visibility, "list")) ||
          !JsonBoolOr(value, "supported_in_api", true))
        continue;
    } else if (!IsResponsesModelId(id)) {
      continue;
    }

    Model model = OpenAI_Model(StrView_Dup(arena, id));
    if (rich) {
      StrView name = Json_GetStr(value, "display_name");
      if (name.Length) model.Name = StrView_Dup(arena, name);
      u32 contextWindow = JsonU32(value, "context_window");
      if (contextWindow) model.ContextWindow = contextWindow;
      JsonValue* efforts = Json_Get(value, "supported_reasoning_levels");
      if (Json_IsArray(efforts)) {
        model.EffortMask = 0;
        for (u32 j = 0; j < efforts->Count; j++) {
          ModelEffort effort =
              ParseEffortOrNone(Json_GetStr(efforts->Items[j], "effort"));
          model.EffortMask |= ModelEffort_Bit(effort);
        }
        model.Reasoning = model.EffortMask != 0;
      }
      ModelEffort defaultEffort =
          ParseEffortOrNone(Json_GetStr(value, "default_reasoning_level"));
      if (Model_SupportsEffort(&model, defaultEffort))
        model.DefaultEffort = defaultEffort;
    }
    out->Models[out->Count++] = model;
  }
  out->Status = ProviderModelListStatus::Success;
  return true;
}
