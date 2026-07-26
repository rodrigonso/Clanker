#include "base/arena.h"
#include "base/str.h"
#include "check.h"
#include "json/json.h"
#include "llm/anthropic.h"

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
  CHECK(StrView_EqCStr(Json_GetStr(j, "system"), "You are pi."));

  JsonValue* arr = Json_Get(j, "messages");
  CHECK(Json_IsArray(arr) && arr->Count == 3);
  CHECK(StrView_EqCStr(Json_GetStr(arr->Items[0], "role"), "user"));

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
  ArenaRelease(&a);
}

void TestAnthropic() {
  TestBuildRequest();
  TestBuildRequestOAuth();
  TestParseSSE();
}
