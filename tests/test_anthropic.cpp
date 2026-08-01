#include "base/arena.h"
#include "base/str.h"
#include "check.h"
#include "json/json.h"
#include "llm/anthropic.h"

static bool HasCacheBreakpoint(JsonValue* block) {
  JsonValue* cc = Json_Get(block, "cache_control");
  return cc != nullptr && StrView_EqCStr(Json_GetStr(cc, "type"), "ephemeral");
}

static JsonValue* LastContentBlock(JsonValue* msg) {
  JsonValue* c = Json_Get(msg, "content");
  CHECK(Json_IsArray(c) && c->Count > 0);
  return c->Items[c->Count - 1];
}

static void TestBuildRequest() {
  Arena a = ArenaCreate();
  Model model = Anthropic_Model(SV("claude-sonnet-4-6"));

  Message msgs[3];
  msgs[0] = Message_User(&a, SV("hi"));
  Message asst = Message_AssistantEmpty(&a);
  Array_Push(&asst.Content, Content_ToolCall(SV("tu1"), SV("read"), SV("{\"path\":\"x\"}")));
  msgs[1] = asst;
  msgs[2] = Message_ToolResult(&a, SV("tu1"), SV("read"), SV("file data"), false);

  ToolDef tool = {SV("read"), SV("Read a file"), SV("{\"type\":\"object\"}")};
  LlmContext ctx = {};
  ctx.SystemPrompt = SV("You are pi.");
  ctx.Messages = msgs;
  ctx.MessageCount = 3;
  ctx.Tools = &tool;
  ctx.ToolCount = 1;
  StreamOptions opt = {};
  opt.MaxTokens = 1000;
  opt.Profile.Effort = ModelEffort::Medium;
  opt.Profile.ContextWindow = 1000000;

  StrBuf body = Anthropic_BuildRequest(&a, &model, &ctx, &opt);
  JsonValue* j = Json_Parse(&a, StrBuf_View(&body));
  CHECK(j != nullptr);
  CHECK_ALL(StrView_EqCStr(Json_GetStr(j, "model"), "claude-sonnet-4-6"),
            Json_GetInt(j, "max_tokens", 0) == 1000,
            Json_GetBool(j, "stream", false) == true);
  JsonValue* outputConfig = Json_Get(j, "output_config");
  CHECK(Json_IsObject(outputConfig));
  CHECK(StrView_EqCStr(Json_GetStr(outputConfig, "effort"), "medium"));
  JsonValue* sys = Json_Get(j, "system");
  CHECK(Json_IsArray(sys) && sys->Count == 1);
  CHECK_ALL(StrView_EqCStr(Json_GetStr(sys->Items[0], "text"), "You are pi."),
            HasCacheBreakpoint(sys->Items[0]));

  JsonValue* arr = Json_Get(j, "messages");
  CHECK(Json_IsArray(arr) && arr->Count == 3);
  CHECK_ALL(StrView_EqCStr(Json_GetStr(arr->Items[0], "role"), "user"),
            !HasCacheBreakpoint(LastContentBlock(arr->Items[0])),
            HasCacheBreakpoint(LastContentBlock(arr->Items[1])),
            HasCacheBreakpoint(LastContentBlock(arr->Items[2])));

  JsonValue* m1 = arr->Items[1];
  CHECK(StrView_EqCStr(Json_GetStr(m1, "role"), "assistant"));
  JsonValue* c1 = Json_Get(m1, "content");
  CHECK(Json_IsArray(c1) && c1->Count == 1);
  CHECK(StrView_EqCStr(Json_GetStr(c1->Items[0], "type"), "tool_use"));
  JsonValue* input = Json_Get(c1->Items[0], "input");
  CHECK(Json_IsObject(input));
  CHECK(StrView_EqCStr(Json_GetStr(input, "path"), "x"));

  JsonValue* m2 = arr->Items[2];
  CHECK(StrView_EqCStr(Json_GetStr(m2, "role"), "user"));
  JsonValue* c2 = Json_Get(m2, "content");
  CHECK(Json_IsArray(c2) && c2->Count == 1);
  CHECK_ALL(StrView_EqCStr(Json_GetStr(c2->Items[0], "type"), "tool_result"),
            StrView_EqCStr(Json_GetStr(c2->Items[0], "tool_use_id"), "tu1"),
            StrView_EqCStr(Json_GetStr(c2->Items[0], "content"), "file data"));

  JsonValue* tools = Json_Get(j, "tools");
  CHECK(Json_IsArray(tools) && tools->Count == 1);
  CHECK(StrView_EqCStr(Json_GetStr(tools->Items[0], "name"), "read"));

  msgs[1].Content.Data[0].ToolArgsJson = SVL("not-json");
  StrBuf sanitized = Anthropic_BuildRequest(&a, &model, &ctx, &opt);
  JsonValue* sanitizedJson = Json_Parse(&a, StrBuf_View(&sanitized));
  CHECK(sanitizedJson != nullptr);
  JsonValue* sanitizedMessages = Json_Get(sanitizedJson, "messages");
  JsonValue* sanitizedContent =
      Json_Get(sanitizedMessages->Items[1], "content");
  CHECK(Json_IsObject(Json_Get(sanitizedContent->Items[0], "input")));

  msgs[1].Content.Data[0].ToolArgsJson = SVL("{\"x\":01}");
  StrBuf canonical = Anthropic_BuildRequest(&a, &model, &ctx, &opt);
  JsonValue* canonicalJson = Json_Parse(&a, StrBuf_View(&canonical));
  CHECK(canonicalJson != nullptr);
  JsonValue* canonicalMessages = Json_Get(canonicalJson, "messages");
  JsonValue* canonicalContent =
      Json_Get(canonicalMessages->Items[1], "content");
  CHECK(Json_GetInt(Json_Get(canonicalContent->Items[0], "input"), "x", 0) ==
        1);

  ArenaRelease(&a);
}

static void TestModelCapabilities() {
  Model sonnet = Anthropic_Model(SVL("claude-sonnet-4-6-20260101"));
  CHECK_ALL(sonnet.ContextWindow == 1000000,
            sonnet.MaxTokens == 8192, sonnet.Reasoning,
            Model_SupportsEffort(&sonnet, ModelEffort::Low),
            Model_SupportsEffort(&sonnet, ModelEffort::Max),
            !Model_SupportsEffort(&sonnet, ModelEffort::XHigh),
            Model_DefaultEffort(&sonnet) == ModelEffort::Medium);

  Model opus = Anthropic_Model(SVL("claude-opus-4-8"));
  CHECK(Model_SupportsEffort(&opus, ModelEffort::XHigh));

  Model unknown = Anthropic_Model(SVL("claude-unknown"));
  CHECK_ALL(unknown.ContextWindow == 0, unknown.EffortMask == 0);

  Model selectable = Anthropic_Model(SVL("claude-unknown"));
  selectable.ContextWindow = 200000;
  selectable.ContextWindowOptions[0] = 200000;
  selectable.ContextWindowOptions[1] = 1000000;
  selectable.ContextWindowOptionCount = 2;
  StreamOptions options = {};
  options.Profile.ContextWindow = 1000000;
  CHECK(StrView_EqCStr(Anthropic_ContextBeta(&selectable, &options),
                       "context-1m-2025-08-07"));
  options.Profile.ContextWindow = 200000;
  CHECK(Anthropic_ContextBeta(&selectable, &options).Length == 0);

  StreamOptions fixed = {};
  fixed.Profile.ContextWindow = sonnet.ContextWindow;
  CHECK(Anthropic_ContextBeta(&sonnet, &fixed).Length == 0);
}

static void TestParseSSE() {
  // Recorded Anthropic SSE: text "Hello world" then a tool_use(read, {"path":"a.txt"}).
  const char* sse =
      "event: message_start\n"
      "data: {\"type\":\"message_start\",\"message\":{\"id\":\"m1\",\"role\":\"assistant\",\"model\":"
      "\"claude\",\"usage\":{\"input_tokens\":10,\"output_tokens\":1}}}\n"
      "\n"
      "event: content_block_start\n"
      "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\","
      "\"text\":\"\"}}\n"
      "\n"
      "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\","
      "\"text\":\"Hello\"}}\n"
      "\n"
      "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\","
      "\"text\":\" world\"}}\n"
      "\n"
      "data: {\"type\":\"content_block_stop\",\"index\":0}\n"
      "\n"
      "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"tool_use\","
      "\"id\":\"toolu_1\",\"name\":\"read\",\"input\":{}}}\n"
      "\n"
      "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"input_json_delta\","
      "\"partial_json\":\"{\\\"path\\\":\"}}\n"
      "\n"
      "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"input_json_delta\","
      "\"partial_json\":\"\\\"a.txt\\\"}\"}}\n"
      "\n"
      "data: {\"type\":\"content_block_stop\",\"index\":1}\n"
      "\n"
      "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"},\"usage\":"
      "{\"output_tokens\":15}}\n"
      "\n"
      "data: {\"type\":\"message_stop\"}\n"
      "\n";

  Arena a = ArenaCreate();
  Message m = Message_AssistantEmpty(&a);
  StreamSink sink = {};
  AnthropicParse st;
  AnthropicParse_Init(&st, &a, &m, sink);

  StrView all = SV(sse);
  for (u32 off = 0; off < all.Length; off += 7) {
    u32 n = ClankMin<u32>(7, all.Length - off);
    AnthropicParse_Feed(&st, StrView{all.Data + off, n});
  }

  CHECK_ALL(m.Content.Count == 2, m.Content.Data[0].Kind == ContentKind::Text,
            StrView_EqCStr(m.Content.Data[0].Text, "Hello world"),
            m.Content.Data[1].Kind == ContentKind::ToolCall,
            StrView_EqCStr(m.Content.Data[1].ToolName, "read"),
            StrView_EqCStr(m.Content.Data[1].ToolArgsJson, "{\"path\":\"a.txt\"}"));
  CHECK_ALL(st.Stop == StopReason::ToolUse, st.Use.Input == 10, st.Use.Output == 15);

  ArenaRelease(&a);
}

static void TestBuildRequestOAuth() {
  Arena a = ArenaCreate();
  Model model = Anthropic_Model(SV("claude-x"));
  Message msgs[1];
  msgs[0] = Message_User(&a, SV("hi"));
  LlmContext ctx = {};
  ctx.SystemPrompt = SV("Be nice.");
  ctx.Messages = msgs;
  ctx.MessageCount = 1;
  StreamOptions opt = {};
  opt.Auth = ProviderAuth{ProviderAuthKind::OAuth,
                          SV("sk-ant-oat01-xxx")};

  StrBuf body = Anthropic_BuildRequest(&a, &model, &ctx, &opt);
  JsonValue* j = Json_Parse(&a, StrBuf_View(&body));
  JsonValue* sys = Json_Get(j, "system");
  CHECK(Json_IsArray(sys) && sys->Count == 2);
  CHECK_ALL(
      StrView_EqCStr(Json_GetStr(sys->Items[0], "text"),
                     "You are Claude Code, Anthropic's official CLI for Claude."),
      StrView_EqCStr(Json_GetStr(sys->Items[1], "text"), "Be nice."),
      !HasCacheBreakpoint(sys->Items[0]), HasCacheBreakpoint(sys->Items[1]));
  ArenaRelease(&a);
}

static void TestBuildRequestCachesToolsWithoutSystem() {
  Arena a = ArenaCreate();
  Model model = Anthropic_Model(SV("claude-x"));
  Message msgs[1];
  msgs[0] = Message_User(&a, SV("hi"));
  ToolDef tools[2] = {{SV("read"), SV("Read a file"), SV("{\"type\":\"object\"}")},
                      {SV("write"), SV("Write a file"), SV("{\"type\":\"object\"}")}};
  LlmContext ctx = {};
  ctx.Messages = msgs;
  ctx.MessageCount = 1;
  ctx.Tools = tools;
  ctx.ToolCount = 2;
  StrBuf body = Anthropic_BuildRequest(&a, &model, &ctx, nullptr);
  JsonValue* j = Json_Parse(&a, StrBuf_View(&body));
  CHECK_ALL(j != nullptr, Json_Get(j, "system") == nullptr);
  JsonValue* tl = Json_Get(j, "tools");
  CHECK(Json_IsArray(tl) && tl->Count == 2);
  CHECK_ALL(!HasCacheBreakpoint(tl->Items[0]),
            HasCacheBreakpoint(tl->Items[1]));
  ArenaRelease(&a);
}

static void TestBuildRequestOmitsIncompleteAssistantMessages() {
  Arena a = ArenaCreate();
  Model model = Anthropic_Model(SV("claude-x"));
  Message msgs[7];
  msgs[0] = Message_User(&a, SVL("before"));
  msgs[1] = Message_AssistantEmpty(&a);
  msgs[2] = Message_AssistantEmpty(&a);
  Array_Push(&msgs[2].Content, Content_Text(SVL(" \t\r\n")));
  msgs[3] = Message_AssistantEmpty(&a);
  msgs[3].Stop = StopReason::Aborted;
  Array_Push(&msgs[3].Content, Content_Text(SVL("partial aborted output")));
  msgs[4] = Message_AssistantEmpty(&a);
  msgs[4].Stop = StopReason::Error;
  Array_Push(&msgs[4].Content, Content_Text(SVL("partial failed output")));
  msgs[5] = Message_User(&a, SVL("after"));
  msgs[6] = Message_AssistantEmpty(&a);
  Array_Push(&msgs[6].Content, Content_Text(SVL("complete")));

  LlmContext ctx = {};
  ctx.Messages = msgs;
  ctx.MessageCount = 7;
  StrBuf body = Anthropic_BuildRequest(&a, &model, &ctx, nullptr);
  JsonValue* j = Json_Parse(&a, StrBuf_View(&body));
  JsonValue* arr = Json_Get(j, "messages");
  CHECK(Json_IsArray(arr) && arr->Count == 3);
  CHECK_ALL(
      StrView_EqCStr(Json_GetStr(arr->Items[0], "role"), "user"),
      StrView_EqCStr(Json_GetStr(LastContentBlock(arr->Items[0]), "text"),
                     "before"),
      !HasCacheBreakpoint(LastContentBlock(arr->Items[0])),
      StrView_EqCStr(Json_GetStr(arr->Items[1], "role"), "user"),
      StrView_EqCStr(Json_GetStr(LastContentBlock(arr->Items[1]), "text"),
                     "after"),
      HasCacheBreakpoint(LastContentBlock(arr->Items[1])),
      StrView_EqCStr(Json_GetStr(arr->Items[2], "role"), "assistant"),
      StrView_EqCStr(Json_GetStr(LastContentBlock(arr->Items[2]), "text"),
                     "complete"),
      HasCacheBreakpoint(LastContentBlock(arr->Items[2])));
  ArenaRelease(&a);
}

static void TestParseModelsResponse() {
  Arena a = ArenaCreate();
  AnthropicModelPage page = {};
  const char* fixture =
      "{\"data\":["
      "{\"id\":\"claude-opus-4-6\",\"type\":\"model\",\"display_name\":\"Claude Opus 4.6\","
      "\"max_input_tokens\":1000000,\"max_tokens\":128000,"
      "\"capabilities\":{\"thinking\":{\"supported\":true}}},"
      "{\"id\":\"claude-basic\",\"display_name\":\"\",\"max_input_tokens\":0,\"max_tokens\":0,"
      "\"capabilities\":{\"thinking\":{\"supported\":false}}}"
      "],\"has_more\":true,\"last_id\":\"claude-basic\"}";
  CHECK(Anthropic_ParseModelsResponse(&a, SV(fixture), &page));
  CHECK_ALL(page.Count == 2, page.HasMore,
            StrView_EqCStr(page.LastId, "claude-basic"));
  CHECK_ALL(StrView_EqCStr(page.Models[0].Id, "claude-opus-4-6"),
            StrView_EqCStr(page.Models[0].Name, "Claude Opus 4.6"),
            page.Models[0].ContextWindow == 1000000, page.Models[0].MaxTokens == 128000,
            page.Models[0].Reasoning,
            StrView_EqCStr(page.Models[0].Provider, "anthropic"),
            StrView_EqCStr(page.Models[0].Api, "anthropic-messages"));
  CHECK_ALL(StrView_EqCStr(page.Models[1].Name, "claude-basic"),
            page.Models[1].ContextWindow == 0, page.Models[1].MaxTokens == 8192,
            !page.Models[1].Reasoning);

  AnthropicModelPage empty = {};
  CHECK(Anthropic_ParseModelsResponse(
      &a, SVL("{\"data\":[],\"has_more\":false,\"last_id\":null}"), &empty));
  CHECK_ALL(empty.Count == 0, empty.Models == nullptr, !empty.HasMore);
  ArenaRelease(&a);
}

static void TestParseModelsResponseRejectsMalformedData() {
  const StrView fixtures[] = {
      SVL("{bad"),
      SVL("{}"),
      SVL("{\"data\":{},\"has_more\":false}"),
      SVL("{\"data\":[],\"has_more\":\"false\"}"),
      SVL("{\"data\":[null],\"has_more\":false}"),
      SVL("{\"data\":[{}],\"has_more\":false}"),
      SVL("{\"data\":[{\"id\":\"x\",\"display_name\":1}],\"has_more\":false}"),
      SVL("{\"data\":[{\"id\":\"x\",\"max_tokens\":1.5}],\"has_more\":false}"),
      SVL("{\"data\":[{\"id\":\"x\",\"capabilities\":{\"thinking\":{\"supported\":\"yes\"}}}],"
          "\"has_more\":false}"),
      SVL("{\"data\":[],\"has_more\":true,\"last_id\":null}"),
  };
  bool rejected = true;
  for (u32 i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++) {
    Arena a = ArenaCreate();
    AnthropicModelPage page = {};
    bool parsed = Anthropic_ParseModelsResponse(&a, fixtures[i], &page);
    rejected = (!parsed && page.Error.Length != 0) && rejected;
    ArenaRelease(&a);
  }
  CHECK(rejected);
}

static void TestMergeModels() {
  Arena a = ArenaCreate();
  Model liveModels[3] = {
      Anthropic_Model(SVL("claude-shared")),
      Anthropic_Model(SVL("claude-live")),
      Anthropic_Model(SVL("claude-shared")),
  };
  liveModels[0].Name = SVL("Live shared");
  liveModels[0].ContextWindow = 999999;
  liveModels[2].Name = SVL("Duplicate ignored");
  ProviderModelList live = {};
  live.Status = ProviderModelListStatus::Success;
  live.Models = liveModels;
  live.Count = 3;

  Model fallback[2] = {
      Anthropic_Model(SVL("claude-shared")),
      Anthropic_Model(SVL("claude-fallback")),
  };
  fallback[0].Name = SVL("Fallback shared");
  fallback[0].EffortMask = ModelEffort_Bit(ModelEffort::High);
  fallback[0].DefaultEffort = ModelEffort::High;
  fallback[1].Name = SVL("Fallback only");
  ProviderModelList merged = ProviderModelList_Merge(&a, live, fallback, 2);
  CHECK_ALL(merged.Status == ProviderModelListStatus::Success, merged.Count == 3,
            StrView_EqCStr(merged.Models[0].Id, "claude-shared"),
            StrView_EqCStr(merged.Models[0].Name, "Live shared"),
            merged.Models[0].ContextWindow == 999999,
            Model_SupportsEffort(&merged.Models[0], ModelEffort::High),
            StrView_EqCStr(merged.Models[1].Id, "claude-live"),
            StrView_EqCStr(merged.Models[2].Id, "claude-fallback"),
            StrView_EqCStr(merged.Models[2].Name, "Fallback only"));

  ProviderModelList failed = {};
  failed.Status = ProviderModelListStatus::Failed;
  failed.Error = SVL("failed");
  ProviderModelList noFallback = ProviderModelList_Merge(&a, failed, fallback, 2);
  CHECK_ALL(noFallback.Status == ProviderModelListStatus::Failed, noFallback.Count == 0,
            StrView_EqCStr(noFallback.Error, "failed"));
  ArenaRelease(&a);
}

void TestAnthropic() {
  TestModelCapabilities();
  TestBuildRequest();
  TestBuildRequestOAuth();
  TestBuildRequestCachesToolsWithoutSystem();
  TestBuildRequestOmitsIncompleteAssistantMessages();
  TestParseSSE();
  TestParseModelsResponse();
  TestParseModelsResponseRejectsMalformedData();
  TestMergeModels();
}
