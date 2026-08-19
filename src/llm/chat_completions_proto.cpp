#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "json/json.h"
#include "llm/chat_completions.h"

namespace {

bool Eq(StrView value, const char* expected) {
  return StrView_EqCStr(value, expected);
}

// Effort values every OpenAI-compatible server understands. Higher tiers exist
// on OpenAI's own API but are rejected elsewhere, so they clamp to "high" --
// the request still asks for maximum reasoning, just in portable wording.
StrView PortableEffort(ModelEffort effort) {
  switch (effort) {
    case ModelEffort::Low: return SVL("low");
    case ModelEffort::Medium: return SVL("medium");
    case ModelEffort::High:
    case ModelEffort::XHigh:
    case ModelEffort::Max:
    case ModelEffort::Ultra: return SVL("high");
    default: return {};
  }
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

bool MessageHasImage(const Message* message) {
  for (u32 i = 0; i < message->Content.Count; i++) {
    if (message->Content.Data[i].Kind == ContentKind::Image) return true;
  }
  return false;
}

// Chat-completions carries one content string per message, so multi-block text
// has to collapse.
StrView JoinText(Arena* arena, const Message* message) {
  StrBuf text = StrBuf_Make(arena, 128);
  for (u32 i = 0; i < message->Content.Count; i++) {
    const Content* content = &message->Content.Data[i];
    if (content->Kind != ContentKind::Text) continue;
    if (text.Length) StrBuf_AppendChar(&text, '\n');
    StrBuf_Append(&text, content->Text);
  }
  return StrBuf_View(&text);
}

void WriteImagePart(StrBuf* out, const Content* content) {
  StrBuf_AppendCStr(out, "{\"type\":\"image_url\",\"image_url\":{\"url\":");
  StrBuf dataUrl = StrBuf_Make(
      out->Backing,
      content->ImageMimeType.Length + content->ImageData.Length + 16);
  StrBuf_AppendCStr(&dataUrl, "data:");
  StrBuf_Append(&dataUrl, content->ImageMimeType);
  StrBuf_AppendCStr(&dataUrl, ";base64,");
  StrBuf_Append(&dataUrl, content->ImageData);
  Json_WriteString(out, StrBuf_View(&dataUrl));
  StrBuf_AppendCStr(out, "}}");
}

// A text-only user turn is written as a plain string rather than a one-element
// array: both are legal, but several compatible servers only accept the string
// form.
void WriteUserMessage(StrBuf* out, Arena* arena, const Message* message) {
  StrBuf_AppendCStr(out, "{\"role\":\"user\",\"content\":");
  if (!MessageHasImage(message)) {
    Json_WriteString(out, JoinText(arena, message));
    StrBuf_AppendChar(out, '}');
    return;
  }
  StrBuf_AppendChar(out, '[');
  bool first = true;
  for (u32 i = 0; i < message->Content.Count; i++) {
    const Content* content = &message->Content.Data[i];
    if (content->Kind == ContentKind::Text) {
      if (!content->Text.Length) continue;
      if (!first) StrBuf_AppendChar(out, ',');
      first = false;
      StrBuf_AppendCStr(out, "{\"type\":\"text\",\"text\":");
      Json_WriteString(out, content->Text);
      StrBuf_AppendChar(out, '}');
    } else if (content->Kind == ContentKind::Image) {
      if (!first) StrBuf_AppendChar(out, ',');
      first = false;
      WriteImagePart(out, content);
    }
  }
  StrBuf_AppendCStr(out, "]}");
}

u32 CountToolCalls(const Message* message) {
  u32 count = 0;
  for (u32 i = 0; i < message->Content.Count; i++) {
    if (message->Content.Data[i].Kind == ContentKind::ToolCall) count++;
  }
  return count;
}

// Thinking blocks are deliberately dropped: chat-completions defines no way to
// return reasoning to the server, and the vendor extensions that exist
// (`reasoning_content`) are not accepted on input. An assistant turn that
// produced only thinking therefore contributes nothing and is skipped.
void WriteAssistantMessage(StrBuf* out, Arena* arena, const Message* message) {
  StrView text = JoinText(arena, message);
  u32 toolCalls = CountToolCalls(message);
  StrBuf_AppendCStr(out, "{\"role\":\"assistant\",\"content\":");
  if (text.Length)
    Json_WriteString(out, text);
  else
    StrBuf_AppendCStr(out, "null");
  if (toolCalls) {
    StrBuf_AppendCStr(out, ",\"tool_calls\":[");
    bool first = true;
    for (u32 i = 0; i < message->Content.Count; i++) {
      const Content* content = &message->Content.Data[i];
      if (content->Kind != ContentKind::ToolCall) continue;
      if (!first) StrBuf_AppendChar(out, ',');
      first = false;
      StrBuf_AppendCStr(out, "{\"id\":");
      Json_WriteString(out, content->ToolId);
      StrBuf_AppendCStr(out, ",\"type\":\"function\",\"function\":{\"name\":");
      Json_WriteString(out, content->ToolName);
      StrBuf_AppendCStr(out, ",\"arguments\":");
      StrBuf canonical =
          StrBuf_Make(out->Backing, content->ToolArgsJson.Length + 16);
      WriteCanonicalObject(&canonical, content->ToolArgsJson, SVL("{}"));
      Json_WriteString(out, StrBuf_View(&canonical));
      StrBuf_AppendCStr(out, "}}");
    }
    StrBuf_AppendChar(out, ']');
  }
  StrBuf_AppendChar(out, '}');
}

// An assistant turn only belongs in the request when it carries something the
// server can act on. Errored and aborted turns never do.
bool AssistantIsSendable(const Message* message) {
  if (message->Stop == StopReason::Error ||
      message->Stop == StopReason::Aborted)
    return false;
  for (u32 i = 0; i < message->Content.Count; i++) {
    ContentKind kind = message->Content.Data[i].Kind;
    if (kind == ContentKind::Text || kind == ContentKind::ToolCall)
      return true;
  }
  return false;
}

void WriteToolMessage(StrBuf* out, Arena* arena, const Message* message) {
  StrBuf_AppendCStr(out, "{\"role\":\"tool\",\"tool_call_id\":");
  Json_WriteString(out, message->ToolCallId);
  StrBuf_AppendCStr(out, ",\"content\":");
  Json_WriteString(out, JoinText(arena, message));
  StrBuf_AppendChar(out, '}');
}

// ---- streaming ----------------------------------------------------------

void Emit(ChatParse* state, StreamEventKind kind, u32 contentIndex,
          StrView delta = {}) {
  StreamEvent event = {};
  event.Kind = kind;
  event.ContentIndex = contentIndex;
  event.Delta = delta;
  StreamSink_Emit(state->Sink, event);
}

StopReason MapFinishReason(StrView reason) {
  if (Eq(reason, "length") || Eq(reason, "max_tokens"))
    return StopReason::Length;
  if (Eq(reason, "tool_calls") || Eq(reason, "function_call"))
    return StopReason::ToolUse;
  if (Eq(reason, "content_filter")) return StopReason::Error;
  return StopReason::Stop;
}

ChatToolSlot* FindToolSlot(ChatParse* state, i64 index) {
  for (u32 i = 0; i < state->Tools.Count; i++) {
    if (state->Tools.Data[i].Index == index) return &state->Tools.Data[i];
  }
  return nullptr;
}

ChatToolSlot* OpenToolSlot(ChatParse* state, i64 index) {
  ChatToolSlot* existing = FindToolSlot(state, index);
  if (existing) return existing;
  ChatToolSlot slot = {};
  slot.Index = index;
  slot.ContentIndex = state->NextContentIndex++;
  slot.Name = StrBuf_Make(state->Mem, 32);
  slot.Arguments = StrBuf_Make(state->Mem, 256);
  ChatToolSlot* pushed = Array_Push(&state->Tools, slot);
  Emit(state, StreamEventKind::ToolCallStart, pushed->ContentIndex);
  return pushed;
}

void AppendText(ChatParse* state, StrView delta) {
  if (!delta.Length) return;
  if (!state->HasText) {
    state->HasText = true;
    state->TextIndex = state->NextContentIndex++;
    Emit(state, StreamEventKind::TextStart, state->TextIndex);
  }
  StrBuf_Append(&state->Text, delta);
  Emit(state, StreamEventKind::TextDelta, state->TextIndex, delta);
}

void AppendReasoning(ChatParse* state, StrView delta) {
  if (!delta.Length) return;
  if (!state->HasReasoning) {
    state->HasReasoning = true;
    state->ReasoningIndex = state->NextContentIndex++;
    Emit(state, StreamEventKind::ThinkingStart, state->ReasoningIndex);
  }
  StrBuf_Append(&state->Reasoning, delta);
  Emit(state, StreamEventKind::ThinkingDelta, state->ReasoningIndex, delta);
}

// `reasoning_content` is DeepSeek's and vLLM's spelling, `reasoning` is
// OpenRouter's. Both are read; neither is ever sent back.
void ReadReasoningDelta(ChatParse* state, JsonValue* delta) {
  JsonValue* value = Json_Get(delta, "reasoning_content");
  if (!Json_IsString(value)) value = Json_Get(delta, "reasoning");
  if (Json_IsString(value)) AppendReasoning(state, value->Str);
}

void ReadToolCallDeltas(ChatParse* state, JsonValue* delta) {
  JsonValue* calls = Json_Get(delta, "tool_calls");
  if (!Json_IsArray(calls)) return;
  for (u32 i = 0; i < calls->Count; i++) {
    JsonValue* call = calls->Items[i];
    if (!Json_IsObject(call)) continue;
    // Servers that omit `index` are only ever streaming one call at a time.
    ChatToolSlot* slot = OpenToolSlot(state, Json_GetInt(call, "index", 0));
    StrView id = Json_GetStr(call, "id");
    if (id.Length && !slot->Id.Length) slot->Id = StrView_Dup(state->Mem, id);
    JsonValue* function = Json_Get(call, "function");
    if (!Json_IsObject(function)) continue;
    StrView name = Json_GetStr(function, "name");
    if (name.Length) StrBuf_Append(&slot->Name, name);
    JsonValue* arguments = Json_Get(function, "arguments");
    if (Json_IsString(arguments) && arguments->Str.Length) {
      StrBuf_Append(&slot->Arguments, arguments->Str);
      Emit(state, StreamEventKind::ToolCallDelta, slot->ContentIndex,
           arguments->Str);
    }
  }
}

void ReadUsage(ChatParse* state, JsonValue* root) {
  JsonValue* usage = Json_Get(root, "usage");
  if (!Json_IsObject(usage)) return;
  state->Use.Input = (u32)Json_GetInt(usage, "prompt_tokens", state->Use.Input);
  state->Use.Output =
      (u32)Json_GetInt(usage, "completion_tokens", state->Use.Output);
  state->Use.TotalTokens =
      (u32)Json_GetInt(usage, "total_tokens", state->Use.TotalTokens);
  JsonValue* promptDetails = Json_Get(usage, "prompt_tokens_details");
  if (Json_IsObject(promptDetails)) {
    state->Use.CacheRead =
        (u32)Json_GetInt(promptDetails, "cached_tokens", state->Use.CacheRead);
    // `prompt_tokens` is inclusive of cached tokens in this API, unlike
    // Anthropic's; subtract so Input keeps meaning "billed at full rate".
    if (state->Use.CacheRead && state->Use.Input >= state->Use.CacheRead)
      state->Use.Input -= state->Use.CacheRead;
  }
  JsonValue* completionDetails = Json_Get(usage, "completion_tokens_details");
  if (Json_IsObject(completionDetails)) {
    state->Use.Reasoning = (u32)Json_GetInt(completionDetails,
                                            "reasoning_tokens",
                                            state->Use.Reasoning);
  }
}

void ReadChoice(ChatParse* state, JsonValue* choice) {
  JsonValue* delta = Json_Get(choice, "delta");
  if (Json_IsObject(delta)) {
    JsonValue* content = Json_Get(delta, "content");
    if (Json_IsString(content)) AppendText(state, content->Str);
    ReadReasoningDelta(state, delta);
    ReadToolCallDeltas(state, delta);
  }
  // Non-streaming servers answering a stream request return a whole `message`.
  JsonValue* message = Json_Get(choice, "message");
  if (Json_IsObject(message)) {
    JsonValue* content = Json_Get(message, "content");
    if (Json_IsString(content)) AppendText(state, content->Str);
    ReadReasoningDelta(state, message);
    ReadToolCallDeltas(state, message);
  }
  StrView finish = Json_GetStr(choice, "finish_reason");
  if (finish.Length) {
    state->SawFinish = true;
    state->Stop = MapFinishReason(finish);
    if (state->Stop == StopReason::Error && !state->SawError) {
      state->SawError = true;
      state->Error = SVL("the response was stopped by a content filter");
    }
  }
}

void SetError(ChatParse* state, StrView message) {
  state->SawError = true;
  state->Stop = StopReason::Error;
  state->Error = StrView_Dup(state->Mem,
                             message.Length
                                 ? message
                                 : SVL("the provider reported an error"));
}

void HandleData(ChatParse* state, StrView data) {
  if (!data.Length) return;
  if (Eq(data, "[DONE]")) {
    state->SawDone = true;
    return;
  }
  // The payload is a slice of the parser's own writable buffer, so it can be
  // null-terminated in place for Json_Parse rather than copied.
  char* end = (char*)(data.Data + data.Length);
  char saved = *end;
  *end = '\0';
  JsonValue* root = Json_Parse(state->Mem, data);
  *end = saved;
  if (!Json_IsObject(root)) return;

  JsonValue* error = Json_Get(root, "error");
  if (Json_IsObject(error)) {
    SetError(state, Json_GetStr(error, "message"));
    return;
  }
  if (Json_IsString(error)) {
    SetError(state, error->Str);
    return;
  }

  JsonValue* choices = Json_Get(root, "choices");
  if (Json_IsArray(choices) && choices->Count) {
    // Agent turns always request a single completion.
    if (Json_IsObject(choices->Items[0])) ReadChoice(state, choices->Items[0]);
  }
  ReadUsage(state, root);
}

bool OnParseLine(void* user, StrView line) {
  ChatParse* state = (ChatParse*)user;
  StrView data = {};
  if (Sse_DataPayload(line, &data)) HandleData(state, data);
  return true;
}

bool IsConversationalModelId(StrView id) {
  const char* incompatible[] = {
      "embed",     "embedding", "image",  "moderation", "audio",
      "realtime",  "tts",       "whisper", "transcribe", "rerank",
      "guard",     "search",
  };
  for (u32 i = 0; i < sizeof(incompatible) / sizeof(incompatible[0]); i++) {
    if (StrView_Contains(id, incompatible[i])) return false;
  }
  return true;
}

void ApplyKnownModelMetadata(Model* model, const ChatEndpoint* endpoint) {
  if (!endpoint || !endpoint->FallbackModels) return;
  for (u32 i = 0; i < endpoint->FallbackModelCount; i++) {
    const Model* known = &endpoint->FallbackModels[i];
    if (!StrView_StartsWith(model->Id, known->Id)) continue;
    model->ContextWindow = known->ContextWindow;
    model->MaxTokens = known->MaxTokens;
    model->Reasoning = known->Reasoning;
    model->EffortMask = known->EffortMask;
    model->DefaultEffort = known->DefaultEffort;
    model->ContextWindowOptionCount = known->ContextWindowOptionCount;
    for (u32 j = 0; j < known->ContextWindowOptionCount; j++)
      model->ContextWindowOptions[j] = known->ContextWindowOptions[j];
    if (StrView_Eq(model->Id, known->Id)) model->Name = known->Name;
    return;
  }
}

}  // namespace

StrView ChatEndpoint_ResolveBaseUrl(Arena* arena, const ChatEndpoint* endpoint) {
  if (!endpoint) return {};
  StrView base = endpoint->BaseUrl;
  if (endpoint->BaseUrlEnvVar) {
    const char* override = getenv(endpoint->BaseUrlEnvVar);
    if (override && *override) base = SV(override);
  }
  while (base.Length && base.Data[base.Length - 1] == '/') base.Length--;
  return arena ? StrView_Dup(arena, base) : base;
}

StrView ChatCompletions_Url(Arena* arena, StrView baseUrl, const char* path) {
  StrView suffix = SV(path);
  StrBuf url = StrBuf_Make(arena, baseUrl.Length + suffix.Length + 8);
  StrBuf_Append(&url, baseUrl);
  bool versioned = baseUrl.Length >= 3 &&
                   memcmp(baseUrl.Data + baseUrl.Length - 3, "/v1", 3) == 0;
  StrBuf_AppendCStr(&url, versioned ? "/" : "/v1/");
  StrBuf_Append(&url, suffix);
  return StrBuf_View(&url);
}

Model ChatCompletions_Model(Arena* arena, const ChatEndpoint* endpoint,
                            StrView modelId) {
  Model model = {};
  model.Id = modelId;
  model.Name = modelId;
  model.Provider = endpoint ? endpoint->Id : StrView{};
  model.BaseUrl = ChatEndpoint_ResolveBaseUrl(arena, endpoint);
  ApplyKnownModelMetadata(&model, endpoint);
  return model;
}

StrBuf ChatCompletions_BuildRequest(Arena* arena, const Model* model,
                                    const LlmContext* context,
                                    const StreamOptions* options) {
  ModelRequestProfile profile = Model_ResolveRequestProfile(
      model, options ? options->Profile : ModelRequestProfile{});
  StrBuf out = StrBuf_Make(arena, 4096);
  StrBuf_AppendCStr(&out, "{\"model\":");
  Json_WriteString(&out, model->Id);
  StrBuf_AppendCStr(
      &out, ",\"stream\":true,\"stream_options\":{\"include_usage\":true}");

  i32 maxTokens = options && options->MaxTokens > 0
                      ? options->MaxTokens
                      : (model->MaxTokens ? (i32)model->MaxTokens : 0);
  if (maxTokens > 0) {
    StrBuf_AppendCStr(&out, ",\"max_tokens\":");
    StrBuf_AppendU64(&out, (u64)maxTokens);
  }

  StrView effort = PortableEffort(profile.Effort);
  if (effort.Length) {
    StrBuf_AppendCStr(&out, ",\"reasoning_effort\":");
    Json_WriteString(&out, effort);
  }

  if (context->ToolCount) {
    StrBuf_AppendCStr(&out,
                      ",\"tool_choice\":\"auto\",\"parallel_tool_calls\":true"
                      ",\"tools\":[");
    for (u32 i = 0; i < context->ToolCount; i++) {
      if (i) StrBuf_AppendChar(&out, ',');
      const ToolDef* tool = &context->Tools[i];
      StrBuf_AppendCStr(&out, "{\"type\":\"function\",\"function\":{\"name\":");
      Json_WriteString(&out, tool->Name);
      StrBuf_AppendCStr(&out, ",\"description\":");
      Json_WriteString(&out, tool->Description);
      StrBuf_AppendCStr(&out, ",\"parameters\":");
      WriteCanonicalObject(&out, tool->ParamsJsonSchema,
                           SVL("{\"type\":\"object\",\"properties\":{}}"));
      StrBuf_AppendCStr(&out, "}}");
    }
    StrBuf_AppendChar(&out, ']');
  }

  StrBuf_AppendCStr(&out, ",\"messages\":[");
  bool first = true;
  if (context->SystemPrompt.Length) {
    first = false;
    StrBuf_AppendCStr(&out, "{\"role\":\"system\",\"content\":");
    Json_WriteString(&out, context->SystemPrompt);
    StrBuf_AppendChar(&out, '}');
  }
  for (u32 i = 0; i < context->MessageCount; i++) {
    const Message* message = &context->Messages[i];
    if (message->Kind == Role::Assistant && !AssistantIsSendable(message))
      continue;
    if (!first) StrBuf_AppendChar(&out, ',');
    first = false;
    if (message->Kind == Role::User)
      WriteUserMessage(&out, arena, message);
    else if (message->Kind == Role::Assistant)
      WriteAssistantMessage(&out, arena, message);
    else
      WriteToolMessage(&out, arena, message);
  }
  StrBuf_AppendCStr(&out, "]}");
  return out;
}

void ChatParse_Init(ChatParse* state, Arena* arena, Message* message,
                    StreamSink sink) {
  *state = {};
  state->Mem = arena;
  state->Buffer = StrBuf_Make(arena, 4096);
  state->Msg = message;
  state->Sink = sink;
  state->Text = StrBuf_Make(arena, 1024);
  state->Reasoning = StrBuf_Make(arena, 256);
  state->Tools = Array_Make<ChatToolSlot>(arena, 4);
  state->Stop = StopReason::Stop;
}

void ChatParse_Feed(ChatParse* state, StrView chunk) {
  StrBuf_Append(&state->Buffer, chunk);
  StrBuf_DrainLines(&state->Buffer, state, OnParseLine);
}

void ChatParse_Finish(ChatParse* state) {
  if (!state || state->Finished) return;
  state->Finished = true;
  if (!state->Msg) return;

  for (u32 index = 0; index < state->NextContentIndex; index++) {
    if (state->HasReasoning && state->ReasoningIndex == index) {
      Array_Push(&state->Msg->Content,
                 Content_Thinking(
                     StrView_Dup(state->Mem, StrBuf_View(&state->Reasoning))));
      Emit(state, StreamEventKind::ThinkingEnd, index);
      continue;
    }
    if (state->HasText && state->TextIndex == index) {
      Array_Push(&state->Msg->Content,
                 Content_Text(
                     StrView_Dup(state->Mem, StrBuf_View(&state->Text))));
      Emit(state, StreamEventKind::TextEnd, index);
      continue;
    }
    for (u32 i = 0; i < state->Tools.Count; i++) {
      ChatToolSlot* slot = &state->Tools.Data[i];
      if (slot->ContentIndex != index || slot->Finished) continue;
      slot->Finished = true;
      StrView arguments = slot->Arguments.Length
                              ? StrView_Dup(state->Mem,
                                            StrBuf_View(&slot->Arguments))
                              : SVL("{}");
      Array_Push(&state->Msg->Content,
                 Content_ToolCall(
                     slot->Id,
                     StrView_Dup(state->Mem, StrBuf_View(&slot->Name)),
                     arguments));
      Emit(state, StreamEventKind::ToolCallEnd, index);
      break;
    }
  }

  // A server may report tool calls without ever setting finish_reason.
  if (state->Tools.Count && state->Stop == StopReason::Stop && !state->SawError)
    state->Stop = StopReason::ToolUse;
}

bool ChatCompletions_ParseModelsResponse(Arena* arena,
                                         const ChatEndpoint* endpoint,
                                         StrView response,
                                         ProviderModelList* out) {
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
    if (!IsConversationalModelId(id)) continue;

    Model model =
        ChatCompletions_Model(arena, endpoint, StrView_Dup(arena, id));
    // OpenRouter and several aggregators publish the real window here.
    u32 contextWindow = (u32)Json_GetInt(value, "context_length", 0);
    if (!contextWindow)
      contextWindow = (u32)Json_GetInt(value, "context_window", 0);
    if (contextWindow) model.ContextWindow = contextWindow;
    StrView name = Json_GetStr(value, "name");
    if (name.Length) model.Name = StrView_Dup(arena, name);
    out->Models[out->Count++] = model;
  }
  out->Status = ProviderModelListStatus::Success;
  return true;
}
