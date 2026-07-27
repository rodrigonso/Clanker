#include <string.h>

#include "json/json.h"
#include "llm/anthropic.h"

// =============================================================================
// Anthropic Messages protocol: request building + SSE parsing. No networking.
// Reference: pi packages/ai/src/api/anthropic-messages.ts.
// =============================================================================

static bool EqC(StrView s, const char* c) { return StrView_EqCStr(s, c); }

// Model metadata (pure data; lives here so tests need not link the HTTP layer).
Model Anthropic_Model(StrView id) {
  Model m = {};
  m.Id = id;
  m.Name = id;
  m.Provider = SVL("anthropic");
  m.Api = SVL("anthropic-messages");
  m.BaseUrl = SVL("https://api.anthropic.com");
  m.ContextWindow = 200000;
  m.MaxTokens = 8192;
  m.Reasoning = false;
  return m;
}

static bool ModelU32(JsonValue* object, const char* key, u32* value, bool* present) {
  JsonValue* field = Json_Get(object, key);
  *present = field != nullptr;
  if (!field) return true;
  if (field->Kind != JsonKind::Number || field->Number < 0.0 || field->Number > 4294967295.0) {
    return false;
  }
  u32 number = (u32)field->Number;
  if ((f64)number != field->Number) return false;
  *value = number;
  return true;
}

bool Anthropic_ParseModelsResponse(Arena* arena, StrView response, AnthropicModelPage* out) {
  *out = {};
  StrBuf input = StrBuf_Make(arena, response.Length + 1);
  StrBuf_Append(&input, response);
  JsonValue* root = Json_Parse(arena, StrBuf_View(&input));
  if (!Json_IsObject(root)) {
    out->Error = SVL("Anthropic models response is not a JSON object");
    return false;
  }

  JsonValue* data = Json_Get(root, "data");
  if (!Json_IsArray(data)) {
    out->Error = SVL("Anthropic models response has no data array");
    return false;
  }
  JsonValue* hasMore = Json_Get(root, "has_more");
  if (!hasMore || hasMore->Kind != JsonKind::Bool) {
    out->Error = SVL("Anthropic models response has no boolean has_more");
    return false;
  }

  out->Models = data->Count ? ArenaPushArray(arena, Model, data->Count) : nullptr;
  for (u32 i = 0; i < data->Count; i++) {
    JsonValue* item = data->Items[i];
    if (!Json_IsObject(item)) {
      out->Error = SVL("Anthropic models response contains a non-object model");
      return false;
    }

    JsonValue* id = Json_Get(item, "id");
    if (!Json_IsString(id) || id->Str.Length == 0) {
      out->Error = SVL("Anthropic model is missing a non-empty id");
      return false;
    }
    JsonValue* type = Json_Get(item, "type");
    if (type && (!Json_IsString(type) || !EqC(type->Str, "model"))) {
      out->Error = SVL("Anthropic model has an invalid type");
      return false;
    }

    Model model = Anthropic_Model(id->Str);
    JsonValue* displayName = Json_Get(item, "display_name");
    if (displayName) {
      if (!Json_IsString(displayName)) {
        out->Error = SVL("Anthropic model has an invalid display_name");
        return false;
      }
      if (displayName->Str.Length) model.Name = displayName->Str;
    }

    u32 maxInput = 0;
    u32 maxOutput = 0;
    bool hasMaxInput = false;
    bool hasMaxOutput = false;
    if (!ModelU32(item, "max_input_tokens", &maxInput, &hasMaxInput) ||
        !ModelU32(item, "max_tokens", &maxOutput, &hasMaxOutput)) {
      out->Error = SVL("Anthropic model has invalid token limits");
      return false;
    }
    if (hasMaxInput && maxInput) model.ContextWindow = maxInput;
    if (hasMaxOutput && maxOutput) model.MaxTokens = maxOutput;

    JsonValue* capabilities = Json_Get(item, "capabilities");
    if (capabilities) {
      if (!Json_IsObject(capabilities)) {
        out->Error = SVL("Anthropic model has invalid capabilities");
        return false;
      }
      JsonValue* thinking = Json_Get(capabilities, "thinking");
      if (thinking) {
        if (!Json_IsObject(thinking)) {
          out->Error = SVL("Anthropic model has invalid thinking capability");
          return false;
        }
        JsonValue* supported = Json_Get(thinking, "supported");
        if (!supported || supported->Kind != JsonKind::Bool) {
          out->Error = SVL("Anthropic model has invalid thinking support");
          return false;
        }
        model.Reasoning = supported->Bool;
      }
    }
    out->Models[out->Count++] = model;
  }

  out->HasMore = hasMore->Bool;
  JsonValue* lastId = Json_Get(root, "last_id");
  if (lastId && lastId->Kind != JsonKind::Null && !Json_IsString(lastId)) {
    out->Error = SVL("Anthropic models response has an invalid last_id");
    return false;
  }
  if (Json_IsString(lastId)) out->LastId = lastId->Str;
  if (out->HasMore && out->LastId.Length == 0) {
    out->Error = SVL("Anthropic models response cannot continue without last_id");
    return false;
  }
  return true;
}

// ---- request building ----------------------------------------------------

// Attach a cache breakpoint to the JSON object just written.
static void MarkCacheBreakpoint(StrBuf* b) {
  FFI(b->Length == 0 || b->Data[b->Length - 1] != '}');
  b->Length--;
  StrBuf_AppendCStr(b, ",\"cache_control\":{\"type\":\"ephemeral\"}}");
}

static void ConcatText(StrBuf* out, const Message* m) {
  for (u32 i = 0; i < m->Content.Count; i++) {
    const Content* c = &m->Content.Data[i];
    if (c->Kind == ContentKind::Text) StrBuf_Append(out, c->Text);
  }
}

static void WriteUserContent(StrBuf* b, const Message* m) {
  bool first = true;
  for (u32 k = 0; k < m->Content.Count; k++) {
    const Content* c = &m->Content.Data[k];
    if (c->Kind == ContentKind::Text) {
      if (c->Text.Length == 0) continue;
      if (!first) StrBuf_AppendChar(b, ',');
      first = false;
      StrBuf_AppendCStr(b, "{\"type\":\"text\",\"text\":");
      Json_WriteString(b, c->Text);
      StrBuf_AppendChar(b, '}');
    } else if (c->Kind == ContentKind::Image) {
      if (!first) StrBuf_AppendChar(b, ',');
      first = false;
      StrBuf_AppendCStr(b, "{\"type\":\"image\",\"source\":{\"type\":\"base64\",\"media_type\":");
      Json_WriteString(b, c->ImageMimeType);
      StrBuf_AppendCStr(b, ",\"data\":");
      Json_WriteString(b, c->ImageData);
      StrBuf_AppendCStr(b, "}}");
    }
  }
  if (first) StrBuf_AppendCStr(b, "{\"type\":\"text\",\"text\":\" \"}");  // never empty
}

static void WriteAssistantContent(StrBuf* b, const Message* m) {
  bool first = true;
  for (u32 k = 0; k < m->Content.Count; k++) {
    const Content* c = &m->Content.Data[k];
    if (c->Kind == ContentKind::Text) {
      if (c->Text.Length == 0) continue;
      if (!first) StrBuf_AppendChar(b, ',');
      first = false;
      StrBuf_AppendCStr(b, "{\"type\":\"text\",\"text\":");
      Json_WriteString(b, c->Text);
      StrBuf_AppendChar(b, '}');
    } else if (c->Kind == ContentKind::ToolCall) {
      if (!first) StrBuf_AppendChar(b, ',');
      first = false;
      StrBuf_AppendCStr(b, "{\"type\":\"tool_use\",\"id\":");
      Json_WriteString(b, c->ToolId);
      StrBuf_AppendCStr(b, ",\"name\":");
      Json_WriteString(b, c->ToolName);
      StrBuf_AppendCStr(b, ",\"input\":");
      if (c->ToolArgsJson.Length)
        StrBuf_Append(b, c->ToolArgsJson);
      else
        StrBuf_AppendCStr(b, "{}");
      StrBuf_AppendChar(b, '}');
    }
  }
  if (first) StrBuf_AppendCStr(b, "{\"type\":\"text\",\"text\":\" \"}");
}

StrBuf Anthropic_BuildRequest(Arena* arena, const Model* model, const LlmContext* ctx,
                              const StreamOptions* opt) {
  StrBuf b = StrBuf_Make(arena, 4096);
  StrBuf_AppendCStr(&b, "{\"model\":");
  Json_WriteString(&b, model->Id);

  i32 maxTok = (opt && opt->MaxTokens > 0) ? opt->MaxTokens : (model->MaxTokens ? (i32)model->MaxTokens : 4096);
  StrBuf_AppendCStr(&b, ",\"max_tokens\":");
  StrBuf_AppendU64(&b, (u64)maxTok);
  StrBuf_AppendCStr(&b, ",\"stream\":true");

  // OAuth (subscription) tokens require the Claude Code identity as the first
  // system block, with the caller's system prompt appended after it.
  bool isOAuth = opt && Anthropic_IsOAuthToken(opt->ApiKey);
  bool haveSystem = isOAuth || ctx->SystemPrompt.Length != 0;
  if (isOAuth) {
    StrBuf_AppendCStr(&b, ",\"system\":[{\"type\":\"text\",\"text\":");
    Json_WriteString(&b, SVL("You are Claude Code, Anthropic's official CLI for Claude."));
    StrBuf_AppendChar(&b, '}');
    if (ctx->SystemPrompt.Length) {
      StrBuf_AppendCStr(&b, ",{\"type\":\"text\",\"text\":");
      Json_WriteString(&b, ctx->SystemPrompt);
      StrBuf_AppendChar(&b, '}');
    }
    // System follows tools in Anthropic's rendered prefix, so this caches both.
    MarkCacheBreakpoint(&b);
    StrBuf_AppendChar(&b, ']');
  } else if (ctx->SystemPrompt.Length) {
    // Block form gives cache_control an object to attach to.
    StrBuf_AppendCStr(&b, ",\"system\":[{\"type\":\"text\",\"text\":");
    Json_WriteString(&b, ctx->SystemPrompt);
    StrBuf_AppendChar(&b, '}');
    MarkCacheBreakpoint(&b);
    StrBuf_AppendChar(&b, ']');
  }

  if (ctx->ToolCount) {
    StrBuf_AppendCStr(&b, ",\"tools\":[");
    for (u32 i = 0; i < ctx->ToolCount; i++) {
      if (i) StrBuf_AppendChar(&b, ',');
      StrBuf_AppendCStr(&b, "{\"name\":");
      Json_WriteString(&b, ctx->Tools[i].Name);
      StrBuf_AppendCStr(&b, ",\"description\":");
      Json_WriteString(&b, ctx->Tools[i].Description);
      StrBuf_AppendCStr(&b, ",\"input_schema\":");
      if (ctx->Tools[i].ParamsJsonSchema.Length)
        StrBuf_Append(&b, ctx->Tools[i].ParamsJsonSchema);
      else
        StrBuf_AppendCStr(&b, "{\"type\":\"object\",\"properties\":{}}");
      StrBuf_AppendChar(&b, '}');
      // With no system prompt the tool list is the entire static prefix, so
      // anchor the breakpoint on its last entry instead.
      if (!haveSystem && i + 1 == ctx->ToolCount) MarkCacheBreakpoint(&b);
    }
    StrBuf_AppendChar(&b, ']');
  }

  // Cache the two most recent emitted messages. Consecutive tool results emit
  // as one message, so walk backward to the start of each group.
  u32 markFrom = ctx->MessageCount;
  for (u32 marked = 0; marked < 2 && markFrom; marked++) {
    markFrom--;
    while (markFrom && ctx->Messages[markFrom].Kind == Role::ToolResult &&
           ctx->Messages[markFrom - 1].Kind == Role::ToolResult)
      markFrom--;
  }

  StrBuf_AppendCStr(&b, ",\"messages\":[");
  bool firstMsg = true;
  u32 i = 0;
  while (i < ctx->MessageCount) {
    const Message* m = &ctx->Messages[i];
    bool markThis = i >= markFrom;
    if (m->Kind == Role::ToolResult) {
      if (!firstMsg) StrBuf_AppendChar(&b, ',');
      firstMsg = false;
      StrBuf_AppendCStr(&b, "{\"role\":\"user\",\"content\":[");
      bool firstBlk = true;
      while (i < ctx->MessageCount && ctx->Messages[i].Kind == Role::ToolResult) {
        const Message* tr = &ctx->Messages[i];
        if (!firstBlk) StrBuf_AppendChar(&b, ',');
        firstBlk = false;
        StrBuf_AppendCStr(&b, "{\"type\":\"tool_result\",\"tool_use_id\":");
        Json_WriteString(&b, tr->ToolCallId);
        StrBuf_AppendCStr(&b, ",\"is_error\":");
        StrBuf_AppendCStr(&b, tr->IsError ? "true" : "false");
        StrBuf_AppendCStr(&b, ",\"content\":");
        StrBuf tmp = StrBuf_Make(arena, 128);
        ConcatText(&tmp, tr);
        Json_WriteString(&b, StrBuf_View(&tmp));
        StrBuf_AppendChar(&b, '}');
        i++;
      }
      if (markThis) MarkCacheBreakpoint(&b);
      StrBuf_AppendCStr(&b, "]}");
      continue;
    }

    if (!firstMsg) StrBuf_AppendChar(&b, ',');
    firstMsg = false;
    if (m->Kind == Role::User) {
      StrBuf_AppendCStr(&b, "{\"role\":\"user\",\"content\":[");
      WriteUserContent(&b, m);
    } else {
      StrBuf_AppendCStr(&b, "{\"role\":\"assistant\",\"content\":[");
      WriteAssistantContent(&b, m);
    }
    if (markThis) MarkCacheBreakpoint(&b);
    StrBuf_AppendCStr(&b, "]}");
    i++;
  }
  StrBuf_AppendCStr(&b, "]}");
  return b;
}

// ---- SSE parsing ---------------------------------------------------------

static void Emit(AnthropicParse* st, StreamEventKind kind, u32 index, StrView delta) {
  StreamEvent ev = {};
  ev.Kind = kind;
  ev.ContentIndex = index;
  ev.Delta = delta;
  StreamSink_Emit(st->Sink, ev);
}

static StopReason MapStop(StrView s) {
  if (EqC(s, "max_tokens")) return StopReason::Length;
  if (EqC(s, "tool_use")) return StopReason::ToolUse;
  return StopReason::Stop;  // end_turn, stop_sequence, etc.
}

static void StartBlock(AnthropicParse* st, i32 idx, JsonValue* cb) {
  st->CurType = 0;
  StrBuf_Clear(&st->CurText);
  StrBuf_Clear(&st->CurSig);
  StrView ct = Json_GetStr(cb, "type");
  if (EqC(ct, "text")) {
    st->CurType = 1;
    Emit(st, StreamEventKind::TextStart, (u32)idx, StrView{"", 0});
  } else if (EqC(ct, "thinking")) {
    st->CurType = 2;
    Emit(st, StreamEventKind::ThinkingStart, (u32)idx, StrView{"", 0});
  } else if (EqC(ct, "tool_use")) {
    st->CurType = 3;
    st->CurToolId = StrView_Dup(st->Mem, Json_GetStr(cb, "id"));
    st->CurToolName = StrView_Dup(st->Mem, Json_GetStr(cb, "name"));
    Emit(st, StreamEventKind::ToolCallStart, (u32)idx, StrView{"", 0});
  }
}

static void BlockDelta(AnthropicParse* st, JsonValue* d) {
  StrView dt = Json_GetStr(d, "type");
  if (EqC(dt, "text_delta")) {
    StrView t = Json_GetStr(d, "text");
    StrBuf_Append(&st->CurText, t);
    Emit(st, StreamEventKind::TextDelta, 0, t);
  } else if (EqC(dt, "thinking_delta")) {
    StrView t = Json_GetStr(d, "thinking");
    StrBuf_Append(&st->CurText, t);
    Emit(st, StreamEventKind::ThinkingDelta, 0, t);
  } else if (EqC(dt, "input_json_delta")) {
    StrBuf_Append(&st->CurText, Json_GetStr(d, "partial_json"));
  } else if (EqC(dt, "signature_delta")) {
    StrBuf_Append(&st->CurSig, Json_GetStr(d, "signature"));
  }
}

static void StopBlock(AnthropicParse* st) {
  if (st->CurType == 1) {
    Array_Push(&st->Msg->Content, Content_Text(StrView_Dup(st->Mem, StrBuf_View(&st->CurText))));
    Emit(st, StreamEventKind::TextEnd, 0, StrView{"", 0});
  } else if (st->CurType == 2) {
    Array_Push(&st->Msg->Content, Content_Thinking(StrView_Dup(st->Mem, StrBuf_View(&st->CurText)),
                                                   StrView_Dup(st->Mem, StrBuf_View(&st->CurSig)), false));
    Emit(st, StreamEventKind::ThinkingEnd, 0, StrView{"", 0});
  } else if (st->CurType == 3) {
    StrView args = st->CurText.Length ? StrView_Dup(st->Mem, StrBuf_View(&st->CurText)) : SVL("{}");
    Array_Push(&st->Msg->Content, Content_ToolCall(st->CurToolId, st->CurToolName, args));
    Emit(st, StreamEventKind::ToolCallEnd, 0, StrView{"", 0});
  }
  st->CurType = 0;
}

static void HandleData(AnthropicParse* st, StrView data) {
  if (data.Length == 0) return;
  // Temporarily null-terminate for the parser (buffer memory is writable).
  char* endp = (char*)(data.Data + data.Length);
  char saved = *endp;
  *endp = '\0';
  JsonValue* j = Json_Parse(st->Mem, data);
  *endp = saved;
  if (!j) return;

  StrView type = Json_GetStr(j, "type");
  if (EqC(type, "message_start")) {
    JsonValue* m = Json_Get(j, "message");
    JsonValue* u = Json_Get(m, "usage");
    st->Use.Input = (u32)Json_GetInt(u, "input_tokens", 0);
    st->Use.CacheRead = (u32)Json_GetInt(u, "cache_read_input_tokens", 0);
    st->Use.CacheWrite = (u32)Json_GetInt(u, "cache_creation_input_tokens", 0);
  } else if (EqC(type, "content_block_start")) {
    StartBlock(st, (i32)Json_GetInt(j, "index", 0), Json_Get(j, "content_block"));
  } else if (EqC(type, "content_block_delta")) {
    BlockDelta(st, Json_Get(j, "delta"));
  } else if (EqC(type, "content_block_stop")) {
    StopBlock(st);
  } else if (EqC(type, "message_delta")) {
    JsonValue* d = Json_Get(j, "delta");
    StrView sr = Json_GetStr(d, "stop_reason");
    if (sr.Length) st->Stop = MapStop(sr);
    JsonValue* u = Json_Get(j, "usage");
    if (u) st->Use.Output = (u32)Json_GetInt(u, "output_tokens", st->Use.Output);
  } else if (EqC(type, "error")) {
    st->SawError = true;
    st->ErrMsg = StrView_Dup(st->Mem, Json_GetStr(Json_Get(j, "error"), "message"));
  }
  // "ping", "message_stop": nothing to do
}

static void HandleLine(AnthropicParse* st, StrView line) {
  if (line.Length < 5 || memcmp(line.Data, "data:", 5) != 0) return;
  StrView data = {line.Data + 5, line.Length - 5};
  while (data.Length && (data.Data[0] == ' ' || data.Data[0] == '\t')) {
    data.Data++;
    data.Length--;
  }
  HandleData(st, data);
}

void AnthropicParse_Init(AnthropicParse* st, Arena* arena, Message* msg, StreamSink sink) {
  *st = {};
  st->Mem = arena;
  st->Buffer = StrBuf_Make(arena, 4096);
  st->Msg = msg;
  st->Sink = sink;
  st->CurText = StrBuf_Make(arena, 1024);
  st->CurSig = StrBuf_Make(arena, 64);
  st->Stop = StopReason::Stop;
}

void AnthropicParse_Feed(AnthropicParse* st, StrView chunk) {
  StrBuf_Append(&st->Buffer, chunk);
  StrBuf* b = &st->Buffer;
  u32 start = 0;
  for (u32 i = 0; i < b->Length; i++) {
    if (b->Data[i] != '\n') continue;
    u32 ll = i - start;
    if (ll > 0 && b->Data[start + ll - 1] == '\r') ll--;
    HandleLine(st, StrView{b->Data + start, ll});
    start = i + 1;
  }
  if (start > 0) {
    u32 rem = b->Length - start;
    memmove(b->Data, b->Data + start, rem);
    b->Length = rem;
    b->Data[rem] = '\0';
  }
}
