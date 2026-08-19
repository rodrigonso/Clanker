#include <string.h>

#include "base/arena.h"
#include "check.h"
#include "llm/chat_completions.h"

namespace {

bool Contains(StrView haystack, const char* needle) {
  return StrView_Contains(haystack, needle);
}

struct EventLog {
  StreamEventKind Kinds[64];
  u32 Indices[64];
  u32 Count;
  StrBuf Text;
  StrBuf Thinking;
  StrBuf Args;
};

void OnEvent(void* user, const StreamEvent& ev) {
  EventLog* log = (EventLog*)user;
  if (log->Count < 64) {
    log->Kinds[log->Count] = ev.Kind;
    log->Indices[log->Count] = ev.ContentIndex;
    log->Count++;
  }
  if (ev.Kind == StreamEventKind::TextDelta) StrBuf_Append(&log->Text, ev.Delta);
  if (ev.Kind == StreamEventKind::ThinkingDelta)
    StrBuf_Append(&log->Thinking, ev.Delta);
  if (ev.Kind == StreamEventKind::ToolCallDelta)
    StrBuf_Append(&log->Args, ev.Delta);
}

u32 CountKind(const EventLog* log, StreamEventKind kind) {
  u32 count = 0;
  for (u32 i = 0; i < log->Count; i++)
    if (log->Kinds[i] == kind) count++;
  return count;
}

EventLog MakeLog(Arena* arena) {
  EventLog log = {};
  log.Text = StrBuf_Make(arena, 256);
  log.Thinking = StrBuf_Make(arena, 256);
  log.Args = StrBuf_Make(arena, 256);
  return log;
}

// Feeds an SSE transcript one byte at a time, which is the harshest framing a
// real socket can produce and the only way to prove the line buffer is right.
void FeedByByte(ChatParse* state, const char* sse) {
  for (const char* p = sse; *p; p++) ChatParse_Feed(state, StrView{p, 1u});
}

Model TestModel() {
  Model model = {};
  model.Id = SVL("test-model");
  model.Name = SVL("Test Model");
  model.Provider = SVL("openai");
  model.BaseUrl = SVL("https://api.openai.com");
  model.ContextWindow = 200000;
  model.Reasoning = true;
  model.EffortMask = MODEL_EFFORT_ALL_MASK;
  model.DefaultEffort = ModelEffort::Medium;
  return model;
}

// ---- URL joining ---------------------------------------------------------

void TestUrls() {
  Arena arena = ArenaCreate();
  CHECK(StrView_EqCStr(
      ChatCompletions_Url(&arena, SVL("https://api.openai.com"), "chat/completions"),
      "https://api.openai.com/v1/chat/completions"));
  // Aggregators and local servers publish an address that already ends in /v1;
  // appending a second one is the most common way to get a 404 here.
  CHECK(StrView_EqCStr(
      ChatCompletions_Url(&arena, SVL("https://openrouter.ai/api/v1"), "models"),
      "https://openrouter.ai/api/v1/models"));
  CHECK(StrView_EqCStr(
      ChatCompletions_Url(&arena, SVL("http://localhost:11434/v1"), "chat/completions"),
      "http://localhost:11434/v1/chat/completions"));

  ChatEndpoint endpoint = {};
  endpoint.BaseUrl = SVL("https://example.test/api/");
  CHECK(StrView_EqCStr(ChatEndpoint_ResolveBaseUrl(&arena, &endpoint),
                       "https://example.test/api"));
  CHECK(ChatEndpoint_ResolveBaseUrl(&arena, nullptr).Length == 0);
  ArenaRelease(&arena);
}

// ---- request building ----------------------------------------------------

void TestRequestBasics() {
  Arena arena = ArenaCreate();
  Model model = TestModel();
  Message messages[] = {Message_User(&arena, SVL("hello \"world\""))};
  LlmContext context = {};
  context.SystemPrompt = SVL("be brief");
  context.Messages = messages;
  context.MessageCount = 1;
  StreamOptions options = {};
  options.MaxTokens = 4096;
  options.Profile.Effort = ModelEffort::Low;

  StrBuf request = ChatCompletions_BuildRequest(&arena, &model, &context, &options);
  StrView body = StrBuf_View(&request);
  CHECK_ALL(Contains(body, "\"model\":\"test-model\""),
            Contains(body, "\"stream\":true"),
            Contains(body, "\"stream_options\":{\"include_usage\":true}"),
            Contains(body, "\"max_tokens\":4096"),
            Contains(body, "\"reasoning_effort\":\"low\""));
  CHECK_ALL(Contains(body, "{\"role\":\"system\",\"content\":\"be brief\"}"),
            Contains(body,
                     "{\"role\":\"user\",\"content\":\"hello \\\"world\\\"\"}"));
  // No tools were offered, so nothing tool-shaped may appear.
  CHECK_ALL(!Contains(body, "\"tools\""), !Contains(body, "\"tool_choice\""),
            !Contains(body, "\"parallel_tool_calls\""));

  // Efforts above the portable triple clamp rather than being sent verbatim.
  options.Profile.Effort = ModelEffort::Ultra;
  StrBuf ultra = ChatCompletions_BuildRequest(&arena, &model, &context, &options);
  CHECK(Contains(StrBuf_View(&ultra), "\"reasoning_effort\":\"high\""));

  // A model that advertises no effort support sends no effort field at all.
  Model plain = model;
  plain.EffortMask = 0;
  plain.DefaultEffort = ModelEffort::None;
  plain.Reasoning = false;
  StreamOptions none = {};
  StrBuf bareBuf = ChatCompletions_BuildRequest(&arena, &plain, &context, &none);
  StrView bare = StrBuf_View(&bareBuf);
  CHECK_ALL(!Contains(bare, "reasoning_effort"), !Contains(bare, "max_tokens"));
  ArenaRelease(&arena);
}

void TestRequestTools() {
  Arena arena = ArenaCreate();
  Model model = TestModel();
  ToolDef tools[2] = {};
  tools[0].Name = SVL("read");
  tools[0].Description = SVL("read a file");
  tools[0].ParamsJsonSchema =
      SVL("{ \"type\" : \"object\", \"properties\" : { \"path\" : { \"type\" : "
          "\"string\" } } }");
  tools[1].Name = SVL("broken");
  tools[1].Description = SVL("bad schema");
  tools[1].ParamsJsonSchema = SVL("not json");

  Message messages[] = {Message_User(&arena, SVL("go"))};
  LlmContext context = {};
  context.Messages = messages;
  context.MessageCount = 1;
  context.Tools = tools;
  context.ToolCount = 2;

  StrBuf request = ChatCompletions_BuildRequest(&arena, &model, &context, nullptr);
  StrView body = StrBuf_View(&request);
  CHECK_ALL(Contains(body, "\"tool_choice\":\"auto\""),
            Contains(body, "\"parallel_tool_calls\":true"),
            Contains(body,
                     "{\"type\":\"function\",\"function\":{\"name\":\"read\""));
  // Schemas are re-serialized canonically, and an unparseable one degrades to
  // an empty object rather than corrupting the request.
  CHECK_ALL(Contains(body,
                     "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\""
                     ":{\"type\":\"string\"}}}"),
            Contains(body,
                     "\"parameters\":{\"type\":\"object\",\"properties\":{}}"));
  ArenaRelease(&arena);
}

void TestRequestMessageShapes() {
  Arena arena = ArenaCreate();
  Model model = TestModel();

  Message withImage = Message_User(&arena, SVL("look"));
  Array_Push(&withImage.Content, Content_Image(SVL("QUJD"), SVL("image/png")));

  Message assistant = Message_AssistantEmpty(&arena);
  Array_Push(&assistant.Content, Content_Thinking(SVL("secret")));
  Array_Push(&assistant.Content, Content_Text(SVL("calling")));
  Array_Push(&assistant.Content,
             Content_ToolCall(SVL("call_1"), SVL("read"),
                              SVL("{\"path\":\"a.txt\"}")));

  Message toolResult =
      Message_ToolResult(&arena, SVL("call_1"), SVL("read"), SVL("body"), false);

  Message errored = Message_AssistantEmpty(&arena);
  Array_Push(&errored.Content, Content_Text(SVL("half a thought")));
  errored.Stop = StopReason::Error;

  Message thinkingOnly = Message_AssistantEmpty(&arena);
  Array_Push(&thinkingOnly.Content, Content_Thinking(SVL("mulling")));

  Message messages[] = {withImage, assistant, toolResult, errored, thinkingOnly};
  LlmContext context = {};
  context.Messages = messages;
  context.MessageCount = 5;

  StrBuf request = ChatCompletions_BuildRequest(&arena, &model, &context, nullptr);
  StrView body = StrBuf_View(&request);
  CHECK_ALL(Contains(body, "{\"type\":\"text\",\"text\":\"look\"}"),
            Contains(body,
                     "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:"
                     "image/png;base64,QUJD\"}}"));
  CHECK_ALL(Contains(body, "{\"role\":\"assistant\",\"content\":\"calling\""),
            Contains(body,
                     "\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\","
                     "\"function\":{\"name\":\"read\",\"arguments\":\"{\\\"path"
                     "\\\":\\\"a.txt\\\"}\"}}]"),
            Contains(body,
                     "{\"role\":\"tool\",\"tool_call_id\":\"call_1\",\"content\""
                     ":\"body\"}"));
  // Thinking never round-trips, and unusable assistant turns are dropped whole.
  CHECK_ALL(!Contains(body, "secret"), !Contains(body, "mulling"),
            !Contains(body, "half a thought"),
            !Contains(body, "reasoning_content"));
  ArenaRelease(&arena);
}

// ---- streaming -----------------------------------------------------------

void TestStreamText() {
  Arena arena = ArenaCreate();
  EventLog log = MakeLog(&arena);
  Message message = Message_AssistantEmpty(&arena);
  ChatParse state = {};
  ChatParse_Init(&state, &arena, &message, StreamSink{&log, OnEvent});

  FeedByByte(&state,
             "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}\n\n"
             "data: {\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}\n\n"
             "data: {\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}\n\n"
             "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
             "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":120,"
             "\"completion_tokens\":8,\"total_tokens\":128,"
             "\"prompt_tokens_details\":{\"cached_tokens\":100},"
             "\"completion_tokens_details\":{\"reasoning_tokens\":3}}}\n\n"
             "data: [DONE]\n\n");
  ChatParse_Finish(&state);

  CHECK_ALL(ChatParse_SawTerminal(&state), state.SawDone, state.SawFinish,
            state.Stop == StopReason::Stop, !state.SawError);
  CHECK_ALL(message.Content.Count == 1,
            StrView_EqCStr(message.Content.Data[0].Text, "Hello"),
            StrView_EqCStr(StrBuf_View(&log.Text), "Hello"));
  CHECK_ALL(CountKind(&log, StreamEventKind::TextStart) == 1,
            CountKind(&log, StreamEventKind::TextEnd) == 1,
            CountKind(&log, StreamEventKind::TextDelta) == 2);
  // prompt_tokens is inclusive of the cache hit in this API; Input must end up
  // meaning "billed at full rate" so context accounting stays honest.
  CHECK_ALL(state.Use.Input == 20, state.Use.CacheRead == 100,
            state.Use.Output == 8, state.Use.Reasoning == 3,
            state.Use.TotalTokens == 128);

  // Finish is idempotent: a second call must not duplicate content.
  ChatParse_Finish(&state);
  CHECK(message.Content.Count == 1);
  ArenaRelease(&arena);
}

void TestStreamReasoningAndTools() {
  Arena arena = ArenaCreate();
  EventLog log = MakeLog(&arena);
  Message message = Message_AssistantEmpty(&arena);
  ChatParse state = {};
  ChatParse_Init(&state, &arena, &message, StreamSink{&log, OnEvent});

  // Two calls interleaved by index, ids and names arriving only on the first
  // frame of each -- the shape OpenAI and vLLM both emit.
  FeedByByte(
      &state,
      "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"think \"}}]}\n\n"
      "data: {\"choices\":[{\"delta\":{\"reasoning\":\"more\"}}]}\n\n"
      "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"}}]}\n\n"
      "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
      "\"id\":\"call_a\",\"function\":{\"name\":\"read\",\"arguments\":\"{\\\"p\"}}]}}]}\n\n"
      "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":1,"
      "\"id\":\"call_b\",\"function\":{\"name\":\"list\",\"arguments\":\"{}\"}}]}}]}\n\n"
      "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
      "\"function\":{\"arguments\":\"\\\":1}\"}}]}}]}\n\n"
      "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
      "data: [DONE]\n\n");
  ChatParse_Finish(&state);

  CHECK_ALL(state.Stop == StopReason::ToolUse, message.Content.Count == 4);
  // Content order follows the order blocks opened, not a fixed convention.
  CHECK_ALL(message.Content.Data[0].Kind == ContentKind::Thinking,
            StrView_EqCStr(message.Content.Data[0].Text, "think more"),
            message.Content.Data[1].Kind == ContentKind::Text,
            StrView_EqCStr(message.Content.Data[1].Text, "ok"));
  CHECK_ALL(message.Content.Data[2].Kind == ContentKind::ToolCall,
            StrView_EqCStr(message.Content.Data[2].ToolId, "call_a"),
            StrView_EqCStr(message.Content.Data[2].ToolName, "read"),
            StrView_EqCStr(message.Content.Data[2].ToolArgsJson, "{\"p\":1}"));
  CHECK_ALL(message.Content.Data[3].Kind == ContentKind::ToolCall,
            StrView_EqCStr(message.Content.Data[3].ToolId, "call_b"),
            StrView_EqCStr(message.Content.Data[3].ToolName, "list"),
            StrView_EqCStr(message.Content.Data[3].ToolArgsJson, "{}"));
  CHECK_ALL(CountKind(&log, StreamEventKind::ToolCallStart) == 2,
            CountKind(&log, StreamEventKind::ToolCallEnd) == 2,
            CountKind(&log, StreamEventKind::ThinkingStart) == 1,
            StrView_EqCStr(StrBuf_View(&log.Thinking), "think more"));
  ArenaRelease(&arena);
}

void TestStreamEdgeCases() {
  // A server that answers a stream request with a whole message, and never
  // sends finish_reason for its tool call.
  {
    Arena arena = ArenaCreate();
    EventLog log = MakeLog(&arena);
    Message message = Message_AssistantEmpty(&arena);
    ChatParse state = {};
    ChatParse_Init(&state, &arena, &message, StreamSink{&log, OnEvent});
    ChatParse_Feed(&state,
                   SVL("data: {\"choices\":[{\"message\":{\"content\":\"whole\","
                       "\"tool_calls\":[{\"id\":\"c1\",\"function\":{\"name\":"
                       "\"read\",\"arguments\":\"{}\"}}]}}]}\n\ndata: [DONE]\n\n"));
    ChatParse_Finish(&state);
    CHECK_ALL(state.Stop == StopReason::ToolUse, message.Content.Count == 2,
              StrView_EqCStr(message.Content.Data[0].Text, "whole"),
              StrView_EqCStr(message.Content.Data[1].ToolId, "c1"));
    ArenaRelease(&arena);
  }

  // Truncation, error frames, and content filtering.
  {
    Arena arena = ArenaCreate();
    EventLog log = MakeLog(&arena);
    Message message = Message_AssistantEmpty(&arena);
    ChatParse state = {};
    ChatParse_Init(&state, &arena, &message, StreamSink{&log, OnEvent});
    ChatParse_Feed(&state,
                   SVL("data: {\"choices\":[{\"delta\":{\"content\":\"a\"},"
                       "\"finish_reason\":\"length\"}]}\n\n"));
    ChatParse_Finish(&state);
    CHECK_ALL(state.Stop == StopReason::Length, state.SawFinish, !state.SawDone,
              ChatParse_SawTerminal(&state));
    ArenaRelease(&arena);
  }
  {
    Arena arena = ArenaCreate();
    EventLog log = MakeLog(&arena);
    Message message = Message_AssistantEmpty(&arena);
    ChatParse state = {};
    ChatParse_Init(&state, &arena, &message, StreamSink{&log, OnEvent});
    ChatParse_Feed(&state,
                   SVL("data: {\"error\":{\"message\":\"rate limited\"}}\n\n"));
    ChatParse_Finish(&state);
    CHECK_ALL(state.SawError, state.Stop == StopReason::Error,
              StrView_EqCStr(state.Error, "rate limited"));
    ArenaRelease(&arena);
  }
  {
    Arena arena = ArenaCreate();
    EventLog log = MakeLog(&arena);
    Message message = Message_AssistantEmpty(&arena);
    ChatParse state = {};
    ChatParse_Init(&state, &arena, &message, StreamSink{&log, OnEvent});
    ChatParse_Feed(&state, SVL("data: {\"choices\":[{\"delta\":{},"
                               "\"finish_reason\":\"content_filter\"}]}\n\n"));
    ChatParse_Finish(&state);
    CHECK_ALL(state.SawError, state.Stop == StopReason::Error,
              state.Error.Length != 0);
    ArenaRelease(&arena);
  }
  // Noise a real stream carries: comments, keepalives, unparseable payloads,
  // and a truncated tail. None may crash or fabricate a terminal state.
  {
    Arena arena = ArenaCreate();
    EventLog log = MakeLog(&arena);
    Message message = Message_AssistantEmpty(&arena);
    ChatParse state = {};
    ChatParse_Init(&state, &arena, &message, StreamSink{&log, OnEvent});
    ChatParse_Feed(&state,
                   SVL(": keepalive\n"
                       "event: message\n"
                       "data: not json\n\n"
                       "data: []\n\n"
                       "data: {\"choices\":[{\"delta\":{\"content\":\"partial\"}}]}\n\n"
                       "data: {\"choices\":[{\"delta\":{\"conte"));
    ChatParse_Finish(&state);
    CHECK_ALL(!ChatParse_SawTerminal(&state), !state.SawError,
              message.Content.Count == 1,
              StrView_EqCStr(message.Content.Data[0].Text, "partial"));
    ArenaRelease(&arena);
  }
}

// ---- model catalog -------------------------------------------------------

void TestModelsCatalog() {
  Arena arena = ArenaCreate();
  const Model known[] = {
      {SVL("known-model"), SVL("Known Model"), SVL("p"), {}, 128000, 8000,
       true, MODEL_EFFORT_STANDARD_MASK, ModelEffort::Medium},
  };
  ChatEndpoint endpoint = {};
  endpoint.Id = SVL("p");
  endpoint.BaseUrl = SVL("https://example.test");
  endpoint.FallbackModels = known;
  endpoint.FallbackModelCount = 1;

  ProviderModelList list = {};
  CHECK(ChatCompletions_ParseModelsResponse(
      &arena, &endpoint,
      SVL("{\"data\":[{\"id\":\"known-model\"},{\"id\":\"known-model-preview\"},"
          "{\"id\":\"text-embedding-3\"},{\"id\":\"tts-1\"},"
          "{\"id\":\"plain\",\"context_window\":32768}]}"),
      &list));
  CHECK_ALL(list.Status == ProviderModelListStatus::Success, list.Count == 3);
  CHECK_ALL(StrView_EqCStr(list.Models[0].Id, "known-model"),
            StrView_EqCStr(list.Models[0].Name, "Known Model"),
            list.Models[0].ContextWindow == 128000,
            list.Models[0].EffortMask == MODEL_EFFORT_STANDARD_MASK);
  // A prefix match inherits capabilities but keeps its own ID as its name.
  CHECK_ALL(StrView_EqCStr(list.Models[1].Id, "known-model-preview"),
            StrView_EqCStr(list.Models[1].Name, "known-model-preview"),
            list.Models[1].ContextWindow == 128000);
  CHECK_ALL(StrView_EqCStr(list.Models[2].Id, "plain"),
            list.Models[2].ContextWindow == 32768,
            list.Models[2].EffortMask == 0);

  ProviderModelList bad = {};
  CHECK(!ChatCompletions_ParseModelsResponse(&arena, &endpoint, SVL("[]"), &bad));
  CHECK(bad.Status == ProviderModelListStatus::Failed);
  CHECK(!ChatCompletions_ParseModelsResponse(&arena, &endpoint,
                                             SVL("{\"data\":[{}]}"), &bad));
  CHECK(bad.Error.Length != 0);
  ArenaRelease(&arena);
}

}  // namespace

void TestChatCompletions() {
  TestUrls();
  TestRequestBasics();
  TestRequestTools();
  TestRequestMessageShapes();
  TestStreamText();
  TestStreamReasoningAndTools();
  TestStreamEdgeCases();
  TestModelsCatalog();
}
