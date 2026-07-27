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
  Model model = Anthropic_Model(SV("claude-x"));

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

  StrBuf body = Anthropic_BuildRequest(&a, &model, &ctx, &opt);
  JsonValue* j = Json_Parse(&a, StrBuf_View(&body));
  CHECK(j != nullptr);
  CHECK(StrView_EqCStr(Json_GetStr(j, "model"), "claude-x"));
  CHECK(Json_GetInt(j, "max_tokens", 0) == 1000);
  CHECK(Json_GetBool(j, "stream", false) == true);
  JsonValue* sys = Json_Get(j, "system");
  CHECK(Json_IsArray(sys) && sys->Count == 1);
  CHECK(StrView_EqCStr(Json_GetStr(sys->Items[0], "text"), "You are pi."));
  CHECK(HasCacheBreakpoint(sys->Items[0]));

  JsonValue* arr = Json_Get(j, "messages");
  CHECK(Json_IsArray(arr) && arr->Count == 3);
  CHECK(StrView_EqCStr(Json_GetStr(arr->Items[0], "role"), "user"));

  CHECK(!HasCacheBreakpoint(LastContentBlock(arr->Items[0])));
  CHECK(HasCacheBreakpoint(LastContentBlock(arr->Items[1])));
  CHECK(HasCacheBreakpoint(LastContentBlock(arr->Items[2])));

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
  CHECK(StrView_EqCStr(Json_GetStr(c2->Items[0], "type"), "tool_result"));
  CHECK(StrView_EqCStr(Json_GetStr(c2->Items[0], "tool_use_id"), "tu1"));
  CHECK(StrView_EqCStr(Json_GetStr(c2->Items[0], "content"), "file data"));

  JsonValue* tools = Json_Get(j, "tools");
  CHECK(Json_IsArray(tools) && tools->Count == 1);
  CHECK(StrView_EqCStr(Json_GetStr(tools->Items[0], "name"), "read"));

  ArenaRelease(&a);
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

  CHECK(m.Content.Count == 2);
  CHECK(m.Content.Data[0].Kind == ContentKind::Text);
  CHECK(StrView_EqCStr(m.Content.Data[0].Text, "Hello world"));
  CHECK(m.Content.Data[1].Kind == ContentKind::ToolCall);
  CHECK(StrView_EqCStr(m.Content.Data[1].ToolName, "read"));
  CHECK(StrView_EqCStr(m.Content.Data[1].ToolArgsJson, "{\"path\":\"a.txt\"}"));
  CHECK(st.Stop == StopReason::ToolUse);
  CHECK(st.Use.Input == 10);
  CHECK(st.Use.Output == 15);

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
  opt.ApiKey = SV("sk-ant-oat01-xxx");  // OAuth token shape

  StrBuf body = Anthropic_BuildRequest(&a, &model, &ctx, &opt);
  JsonValue* j = Json_Parse(&a, StrBuf_View(&body));
  JsonValue* sys = Json_Get(j, "system");
  CHECK(Json_IsArray(sys) && sys->Count == 2);
  CHECK(StrView_EqCStr(Json_GetStr(sys->Items[0], "text"),
                       "You are Claude Code, Anthropic's official CLI for Claude."));
  CHECK(StrView_EqCStr(Json_GetStr(sys->Items[1], "text"), "Be nice."));
  CHECK(!HasCacheBreakpoint(sys->Items[0]));
  CHECK(HasCacheBreakpoint(sys->Items[1]));
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
  CHECK(j != nullptr);
  CHECK(Json_Get(j, "system") == nullptr);
  JsonValue* tl = Json_Get(j, "tools");
  CHECK(Json_IsArray(tl) && tl->Count == 2);
  CHECK(!HasCacheBreakpoint(tl->Items[0]));
  CHECK(HasCacheBreakpoint(tl->Items[1]));
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
  CHECK(page.Count == 2);
  CHECK(page.HasMore);
  CHECK(StrView_EqCStr(page.LastId, "claude-basic"));
  CHECK(StrView_EqCStr(page.Models[0].Id, "claude-opus-4-6"));
  CHECK(StrView_EqCStr(page.Models[0].Name, "Claude Opus 4.6"));
  CHECK(page.Models[0].ContextWindow == 1000000);
  CHECK(page.Models[0].MaxTokens == 128000);
  CHECK(page.Models[0].Reasoning);
  CHECK(StrView_EqCStr(page.Models[0].Provider, "anthropic"));
  CHECK(StrView_EqCStr(page.Models[0].Api, "anthropic-messages"));
  CHECK(StrView_EqCStr(page.Models[1].Name, "claude-basic"));
  CHECK(page.Models[1].ContextWindow == 200000);
  CHECK(page.Models[1].MaxTokens == 8192);
  CHECK(!page.Models[1].Reasoning);

  AnthropicModelPage empty = {};
  CHECK(Anthropic_ParseModelsResponse(
      &a, SVL("{\"data\":[],\"has_more\":false,\"last_id\":null}"), &empty));
  CHECK(empty.Count == 0);
  CHECK(empty.Models == nullptr);
  CHECK(!empty.HasMore);
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
  for (u32 i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++) {
    Arena a = ArenaCreate();
    AnthropicModelPage page = {};
    CHECK(!Anthropic_ParseModelsResponse(&a, fixtures[i], &page));
    CHECK(page.Error.Length != 0);
    ArenaRelease(&a);
  }
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
  fallback[1].Name = SVL("Fallback only");
  ProviderModelList merged = ProviderModelList_Merge(&a, live, fallback, 2);
  CHECK(merged.Status == ProviderModelListStatus::Success);
  CHECK(merged.Count == 3);
  CHECK(StrView_EqCStr(merged.Models[0].Id, "claude-shared"));
  CHECK(StrView_EqCStr(merged.Models[0].Name, "Live shared"));
  CHECK(merged.Models[0].ContextWindow == 999999);
  CHECK(StrView_EqCStr(merged.Models[1].Id, "claude-live"));
  CHECK(StrView_EqCStr(merged.Models[2].Id, "claude-fallback"));
  CHECK(StrView_EqCStr(merged.Models[2].Name, "Fallback only"));

  ProviderModelList failed = {};
  failed.Status = ProviderModelListStatus::Failed;
  failed.Error = SVL("failed");
  ProviderModelList noFallback = ProviderModelList_Merge(&a, failed, fallback, 2);
  CHECK(noFallback.Status == ProviderModelListStatus::Failed);
  CHECK(noFallback.Count == 0);
  CHECK(StrView_EqCStr(noFallback.Error, "failed"));
  ArenaRelease(&a);
}

void TestAnthropic() {
  TestBuildRequest();
  TestBuildRequestOAuth();
  TestBuildRequestCachesToolsWithoutSystem();
  TestParseSSE();
  TestParseModelsResponse();
  TestParseModelsResponseRejectsMalformedData();
  TestMergeModels();
}
