#include <string.h>

#include "base/arena.h"
#include "check.h"
#include "llm/responses.h"

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
void FeedByByte(ResponsesParse* state, const char* sse) {
  for (const char* p = sse; *p; p++) ResponsesParse_Feed(state, StrView{p, 1u});
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
      Responses_Url(&arena, SVL("https://api.openai.com"), "responses"),
      "https://api.openai.com/v1/responses"));
  // A gateway addressed through OPENAI_BASE_URL usually publishes an address
  // that already ends in /v1; appending a second one is the most common way to
  // get a 404 here.
  CHECK(StrView_EqCStr(
      Responses_Url(&arena, SVL("https://gateway.test/api/v1"), "models"),
      "https://gateway.test/api/v1/models"));
  CHECK(StrView_EqCStr(
      Responses_Url(&arena, SVL("http://localhost:8000/v1"), "responses"),
      "http://localhost:8000/v1/responses"));

  ResponsesEndpoint endpoint = {};
  endpoint.BaseUrl = SVL("https://example.test/api/");
  CHECK(StrView_EqCStr(ResponsesEndpoint_ResolveBaseUrl(&arena, &endpoint),
                       "https://example.test/api"));
  CHECK(ResponsesEndpoint_ResolveBaseUrl(&arena, nullptr).Length == 0);
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

  StrBuf request = Responses_BuildRequest(&arena, &model, &context, &options);
  StrView body = StrBuf_View(&request);
  CHECK_ALL(Contains(body, "\"model\":\"test-model\""),
            Contains(body, "\"store\":false"), Contains(body, "\"stream\":true"),
            Contains(body, "\"instructions\":\"be brief\""),
            Contains(body, "\"max_output_tokens\":4096"));
  // Reasoning must be requested with a summary, and asked back encrypted --
  // without the include there is nothing to replay on the next turn.
  CHECK_ALL(
      Contains(body, "\"reasoning\":{\"effort\":\"low\",\"summary\":\"auto\"}"),
      Contains(body, "\"include\":[\"reasoning.encrypted_content\"]"));
  CHECK(Contains(body,
                 "{\"type\":\"message\",\"role\":\"user\",\"content\":"
                 "[{\"type\":\"input_text\",\"text\":\"hello \\\"world\\\"\"}]}"));

  // "ultra" is Clank's name for the tier the wire calls "max".
  options.Profile.Effort = ModelEffort::Ultra;
  StrBuf ultra = Responses_BuildRequest(&arena, &model, &context, &options);
  CHECK(Contains(StrBuf_View(&ultra), "\"reasoning\":{\"effort\":\"max\""));

  options.Profile.Effort = ModelEffort::XHigh;
  StrBuf xhigh = Responses_BuildRequest(&arena, &model, &context, &options);
  CHECK(Contains(StrBuf_View(&xhigh), "\"reasoning\":{\"effort\":\"xhigh\""));

  // A model that advertises no effort support sends no reasoning block at all.
  Model plain = model;
  plain.EffortMask = 0;
  plain.DefaultEffort = ModelEffort::None;
  plain.Reasoning = false;
  StreamOptions none = {};
  StrBuf bareBuf = Responses_BuildRequest(&arena, &plain, &context, &none);
  StrView bare = StrBuf_View(&bareBuf);
  CHECK_ALL(!Contains(bare, "\"reasoning\":{"),
            !Contains(bare, "max_output_tokens"));
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
  StreamOptions options = {};
  options.Profile.Effort = ModelEffort::High;

  StrBuf request = Responses_BuildRequest(&arena, &model, &context, &options);
  StrView body = StrBuf_View(&request);
  // The regression this whole endpoint exists for: function tools and a
  // reasoning effort in one request, which chat-completions rejects.
  CHECK_ALL(Contains(body, "\"reasoning\":{\"effort\":\"high\""),
            Contains(body, "\"tool_choice\":\"auto\""),
            Contains(body, "\"parallel_tool_calls\":true"));
  // Responses declares a function tool flat, not nested under "function".
  CHECK_ALL(Contains(body, "{\"type\":\"function\",\"name\":\"read\""),
            Contains(body, "\"strict\":false"),
            !Contains(body, "\"function\":{\"name\""));
  // Schemas are re-serialized canonically, and an unparseable one degrades to
  // an empty object rather than corrupting the request.
  CHECK_ALL(Contains(body,
                     "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\""
                     ":{\"type\":\"string\"}}}"),
            Contains(body,
                     "\"parameters\":{\"type\":\"object\",\"properties\":{}}"));

  // A turn with no tools still declares the rest of its shape.
  context.ToolCount = 0;
  StrBuf toolless = Responses_BuildRequest(&arena, &model, &context, &options);
  CHECK(!Contains(StrBuf_View(&toolless), "\"tools\":["));
  ArenaRelease(&arena);
}

void TestRequestMessageShapes() {
  Arena arena = ArenaCreate();
  Model model = TestModel();

  Message withImage = Message_User(&arena, SVL("look"));
  Array_Push(&withImage.Content, Content_Image(SVL("QUJD"), SVL("image/png")));

  Message assistant = Message_AssistantEmpty(&arena);
  assistant.Provider = SVL("openai");
  assistant.Model = SVL("test-model");
  Content thinking = Content_Thinking(SVL("summary text"));
  thinking.Signature =
      SVL("{\"type\":\"reasoning\",\"id\":\"rs_1\",\"encrypted_content\":\"xx\"}");
  Array_Push(&assistant.Content, thinking);
  Content text = Content_Text(SVL("calling"));
  text.Signature = SVL("{\"id\":\"msg_1\",\"phase\":\"final\"}");
  Array_Push(&assistant.Content, text);
  Content call = Content_ToolCall(SVL("call_1"), SVL("read"),
                                  SVL("{\"path\":\"a.txt\"}"));
  call.Signature = SVL("fc_1");
  Array_Push(&assistant.Content, call);

  Message toolResult =
      Message_ToolResult(&arena, SVL("call_1"), SVL("read"), SVL("body"), false);

  Message errored = Message_AssistantEmpty(&arena);
  Array_Push(&errored.Content, Content_Text(SVL("half a thought")));
  errored.Stop = StopReason::Error;

  // Reasoning with no signature cannot be reconstructed, so the turn carries
  // nothing the server can act on.
  Message thinkingOnly = Message_AssistantEmpty(&arena);
  thinkingOnly.Provider = SVL("openai");
  thinkingOnly.Model = SVL("test-model");
  Array_Push(&thinkingOnly.Content, Content_Thinking(SVL("mulling")));

  Message messages[] = {withImage, assistant, toolResult, errored, thinkingOnly};
  LlmContext context = {};
  context.Messages = messages;
  context.MessageCount = 5;

  StrBuf request = Responses_BuildRequest(&arena, &model, &context, nullptr);
  StrView body = StrBuf_View(&request);
  CHECK_ALL(Contains(body, "{\"type\":\"input_text\",\"text\":\"look\"}"),
            Contains(body,
                     "{\"type\":\"input_image\",\"detail\":\"auto\",\"image_url\":"
                     "\"data:image/png;base64,QUJD\"}"));
  // The reasoning item is replayed verbatim, encrypted content and all.
  CHECK(Contains(body,
                 "{\"type\":\"reasoning\",\"id\":\"rs_1\",\"encrypted_content\""
                 ":\"xx\"}"));
  CHECK_ALL(Contains(body,
                     "{\"type\":\"message\",\"role\":\"assistant\",\"status\":"
                     "\"completed\",\"id\":\"msg_1\",\"phase\":\"final\""),
            Contains(body,
                     "{\"type\":\"output_text\",\"text\":\"calling\","
                     "\"annotations\":[]}"));
  // `arguments` is a JSON string holding JSON, and the item id rides alongside
  // the call id.
  CHECK(Contains(body,
                 "{\"type\":\"function_call\",\"id\":\"fc_1\",\"call_id\":"
                 "\"call_1\",\"name\":\"read\",\"arguments\":\"{\\\"path\\\":"
                 "\\\"a.txt\\\"}\"}"));
  CHECK(Contains(body,
                 "{\"type\":\"function_call_output\",\"call_id\":\"call_1\","
                 "\"output\":\"body\"}"));
  CHECK_ALL(!Contains(body, "mulling"), !Contains(body, "half a thought"));

  // Item ids are scoped to the model that issued them. Against a different
  // model the turn still goes, but under a generated id and with no reasoning.
  Model other = model;
  other.Id = SVL("other-model");
  StrBuf foreign = Responses_BuildRequest(&arena, &other, &context, nullptr);
  StrView foreignBody = StrBuf_View(&foreign);
  CHECK_ALL(!Contains(foreignBody, "rs_1"), !Contains(foreignBody, "msg_1"),
            !Contains(foreignBody, "\"id\":\"fc_1\""),
            Contains(foreignBody, "\"id\":\"msg_clank_1_1\""),
            Contains(foreignBody, "\"text\":\"calling\""));
  ArenaRelease(&arena);
}

// ---- streaming -----------------------------------------------------------

void TestStreamText() {
  Arena arena = ArenaCreate();
  EventLog log = MakeLog(&arena);
  Message message = Message_AssistantEmpty(&arena);
  ResponsesParse state = {};
  ResponsesParse_Init(&state, &arena, &message, StreamSink{&log, OnEvent});

  FeedByByte(
      &state,
      "event: response.created\n"
      "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_1\"}}\n\n"
      "data: {\"type\":\"response.output_item.added\",\"output_index\":0,"
      "\"item\":{\"type\":\"message\",\"id\":\"msg_1\",\"phase\":\"final\"}}\n\n"
      "data: {\"type\":\"response.output_text.delta\",\"output_index\":0,"
      "\"delta\":\"Hel\"}\n\n"
      "data: {\"type\":\"response.output_text.delta\",\"output_index\":0,"
      "\"delta\":\"lo\"}\n\n"
      "data: {\"type\":\"response.output_item.done\",\"output_index\":0,"
      "\"item\":{\"type\":\"message\",\"id\":\"msg_1\",\"phase\":\"final\","
      "\"content\":[{\"type\":\"output_text\",\"text\":\"Hello\"}]}}\n\n"
      "data: {\"type\":\"response.completed\",\"response\":{\"usage\":"
      "{\"input_tokens\":120,\"output_tokens\":8,\"total_tokens\":128,"
      "\"input_tokens_details\":{\"cached_tokens\":100},"
      "\"output_tokens_details\":{\"reasoning_tokens\":3}}}}\n\n");
  ResponsesParse_Finish(&state);

  CHECK_ALL(ResponsesParse_SawTerminal(&state), state.Stop == StopReason::Stop,
            !state.SawError);
  CHECK_ALL(message.Content.Count == 1,
            StrView_EqCStr(message.Content.Data[0].Text, "Hello"),
            StrView_EqCStr(StrBuf_View(&log.Text), "Hello"));
  // The item's identity is kept so the next request can replay it.
  CHECK(StrView_EqCStr(message.Content.Data[0].Signature,
                       "{\"id\":\"msg_1\",\"phase\":\"final\"}"));
  CHECK_ALL(CountKind(&log, StreamEventKind::TextStart) == 1,
            CountKind(&log, StreamEventKind::TextEnd) == 1,
            CountKind(&log, StreamEventKind::TextDelta) == 2);
  // input_tokens is inclusive of the cache hit; Input must end up meaning
  // "billed at full rate" so context accounting stays honest.
  CHECK_ALL(state.Use.Input == 20, state.Use.CacheRead == 100,
            state.Use.Output == 8, state.Use.Reasoning == 3,
            state.Use.TotalTokens == 128);

  // Finish is idempotent: a second call must not duplicate content.
  ResponsesParse_Finish(&state);
  CHECK(message.Content.Count == 1);
  ArenaRelease(&arena);
}

void TestStreamReasoningAndTools() {
  Arena arena = ArenaCreate();
  EventLog log = MakeLog(&arena);
  Message message = Message_AssistantEmpty(&arena);
  ResponsesParse state = {};
  ResponsesParse_Init(&state, &arena, &message, StreamSink{&log, OnEvent});

  FeedByByte(
      &state,
      "data: {\"type\":\"response.output_item.added\",\"output_index\":0,"
      "\"item\":{\"type\":\"reasoning\",\"id\":\"rs_1\"}}\n\n"
      "data: {\"type\":\"response.reasoning_summary_text.delta\","
      "\"output_index\":0,\"delta\":\"think\"}\n\n"
      "data: {\"type\":\"response.reasoning_summary_part.done\","
      "\"output_index\":0}\n\n"
      "data: {\"type\":\"response.reasoning_summary_text.delta\","
      "\"output_index\":0,\"delta\":\"more\"}\n\n"
      "data: {\"type\":\"response.output_item.done\",\"output_index\":0,"
      "\"item\":{\"type\":\"reasoning\",\"id\":\"rs_1\","
      "\"encrypted_content\":\"ENC\",\"summary\":[{\"type\":"
      "\"summary_text\",\"text\":\"think\"},{\"type\":\"summary_text\","
      "\"text\":\"more\"}]}}\n\n"
      "data: {\"type\":\"response.output_item.added\",\"output_index\":1,"
      "\"item\":{\"type\":\"function_call\",\"id\":\"fc_1\",\"call_id\":"
      "\"call_a\",\"name\":\"read\"}}\n\n"
      "data: {\"type\":\"response.function_call_arguments.delta\","
      "\"output_index\":1,\"delta\":\"{\\\"p\"}\n\n"
      "data: {\"type\":\"response.function_call_arguments.delta\","
      "\"output_index\":1,\"delta\":\"\\\":1}\"}\n\n"
      "data: {\"type\":\"response.output_item.done\",\"output_index\":1,"
      "\"item\":{\"type\":\"function_call\",\"id\":\"fc_1\",\"call_id\":"
      "\"call_a\",\"name\":\"read\",\"arguments\":\"{\\\"p\\\":1}\"}}\n\n"
      "data: {\"type\":\"response.completed\",\"response\":{}}\n\n");
  ResponsesParse_Finish(&state);

  CHECK_ALL(state.Stop == StopReason::ToolUse, state.SawTool,
            message.Content.Count == 2);
  CHECK_ALL(message.Content.Data[0].Kind == ContentKind::Thinking,
            StrView_EqCStr(message.Content.Data[0].Text, "think\n\nmore"));
  // The whole reasoning item is kept verbatim: it is the only thing that can
  // be handed back, and it cannot be rebuilt from the summary text.
  CHECK_ALL(Contains(message.Content.Data[0].Signature, "\"encrypted_content\""),
            Contains(message.Content.Data[0].Signature, "ENC"),
            Contains(message.Content.Data[0].Signature, "\"type\":\"reasoning\""));
  CHECK_ALL(message.Content.Data[1].Kind == ContentKind::ToolCall,
            StrView_EqCStr(message.Content.Data[1].ToolId, "call_a"),
            StrView_EqCStr(message.Content.Data[1].ToolName, "read"),
            StrView_EqCStr(message.Content.Data[1].ToolArgsJson, "{\"p\":1}"),
            StrView_EqCStr(message.Content.Data[1].Signature, "fc_1"));
  CHECK_ALL(CountKind(&log, StreamEventKind::ThinkingStart) == 1,
            CountKind(&log, StreamEventKind::ThinkingEnd) == 1,
            CountKind(&log, StreamEventKind::ToolCallStart) == 1,
            CountKind(&log, StreamEventKind::ToolCallEnd) == 1,
            StrView_EqCStr(StrBuf_View(&log.Thinking), "think\n\nmore"),
            StrView_EqCStr(StrBuf_View(&log.Args), "{\"p\":1}"));
  ArenaRelease(&arena);
}

void TestStreamEdgeCases() {
  // Hitting the output cap is truncation, not failure.
  {
    Arena arena = ArenaCreate();
    EventLog log = MakeLog(&arena);
    Message message = Message_AssistantEmpty(&arena);
    ResponsesParse state = {};
    ResponsesParse_Init(&state, &arena, &message, StreamSink{&log, OnEvent});
    ResponsesParse_Feed(
        &state,
        SVL("data: {\"type\":\"response.incomplete\",\"response\":"
            "{\"incomplete_details\":{\"reason\":\"max_output_tokens\"}}}\n\n"));
    ResponsesParse_Finish(&state);
    CHECK_ALL(state.Stop == StopReason::Length, !state.SawError,
              ResponsesParse_SawTerminal(&state));
    ArenaRelease(&arena);
  }
  // Any other incomplete reason is an error.
  {
    Arena arena = ArenaCreate();
    EventLog log = MakeLog(&arena);
    Message message = Message_AssistantEmpty(&arena);
    ResponsesParse state = {};
    ResponsesParse_Init(&state, &arena, &message, StreamSink{&log, OnEvent});
    ResponsesParse_Feed(
        &state,
        SVL("data: {\"type\":\"response.incomplete\",\"response\":"
            "{\"incomplete_details\":{\"reason\":\"content_filter\"}}}\n\n"));
    ResponsesParse_Finish(&state);
    CHECK_ALL(state.SawError, state.Stop == StopReason::Error,
              StrView_EqCStr(state.Error, "content_filter"));
    ArenaRelease(&arena);
  }
  {
    Arena arena = ArenaCreate();
    EventLog log = MakeLog(&arena);
    Message message = Message_AssistantEmpty(&arena);
    ResponsesParse state = {};
    ResponsesParse_Init(&state, &arena, &message, StreamSink{&log, OnEvent});
    ResponsesParse_Feed(&state,
                        SVL("data: {\"type\":\"response.failed\",\"response\":"
                            "{\"error\":{\"message\":\"rate limited\"}}}\n\n"));
    ResponsesParse_Finish(&state);
    CHECK_ALL(state.SawError, ResponsesParse_SawTerminal(&state),
              StrView_EqCStr(state.Error, "rate limited"));
    ArenaRelease(&arena);
  }
  {
    Arena arena = ArenaCreate();
    EventLog log = MakeLog(&arena);
    Message message = Message_AssistantEmpty(&arena);
    ResponsesParse state = {};
    ResponsesParse_Init(&state, &arena, &message, StreamSink{&log, OnEvent});
    ResponsesParse_Feed(&state, SVL("data: {\"type\":\"error\",\"message\":"
                                    "\"stream broke\"}\n\n"));
    ResponsesParse_Finish(&state);
    CHECK_ALL(state.SawError, state.Stop == StopReason::Error,
              !ResponsesParse_SawTerminal(&state),
              StrView_EqCStr(state.Error, "stream broke"));
    ArenaRelease(&arena);
  }
  // Noise a real stream carries: comments, keepalives, unparseable payloads,
  // an unknown item type, and a truncated tail. None may crash or fabricate a
  // terminal state, and whatever text arrived before the cut is kept.
  {
    Arena arena = ArenaCreate();
    EventLog log = MakeLog(&arena);
    Message message = Message_AssistantEmpty(&arena);
    ResponsesParse state = {};
    ResponsesParse_Init(&state, &arena, &message, StreamSink{&log, OnEvent});
    ResponsesParse_Feed(
        &state,
        SVL(": keepalive\n"
            "event: response.output_text.delta\n"
            "data: not json\n\n"
            "data: []\n\n"
            "data: {\"type\":\"response.output_item.added\",\"output_index\":7,"
            "\"item\":{\"type\":\"web_search_call\",\"id\":\"ws_1\"}}\n\n"
            "data: {\"type\":\"response.output_item.added\",\"output_index\":0,"
            "\"item\":{\"type\":\"message\",\"id\":\"msg_1\"}}\n\n"
            "data: {\"type\":\"response.output_text.delta\",\"output_index\":0,"
            "\"delta\":\"partial\"}\n\n"
            "data: {\"type\":\"response.output_te"));
    ResponsesParse_Finish(&state);
    CHECK_ALL(!ResponsesParse_SawTerminal(&state), !state.SawError,
              message.Content.Count == 1,
              StrView_EqCStr(message.Content.Data[0].Text, "partial"),
              CountKind(&log, StreamEventKind::TextEnd) == 1);
    ArenaRelease(&arena);
  }
}

// ---- model catalog -------------------------------------------------------

void TestModelsCatalog() {
  Arena arena = ArenaCreate();
  const Model known[] = {
      {SVL("gpt-known"), SVL("Known Model"), SVL("p"), {}, 128000, 8000, true,
       MODEL_EFFORT_STANDARD_MASK, ModelEffort::Medium},
  };
  ResponsesEndpoint endpoint = {};
  endpoint.Id = SVL("p");
  endpoint.BaseUrl = SVL("https://example.test");
  endpoint.FallbackModels = known;
  endpoint.FallbackModelCount = 1;

  ProviderModelList list = {};
  CHECK(Responses_ParseModelsResponse(
      &arena, &endpoint,
      SVL("{\"data\":[{\"id\":\"gpt-known\"},{\"id\":\"gpt-known-preview\"},"
          "{\"id\":\"text-embedding-3\"},{\"id\":\"tts-1\"},"
          "{\"id\":\"gpt-audio\"},{\"id\":\"claude-opus\"},"
          "{\"id\":\"o3-pro\",\"context_window\":32768}]}"),
      &list));
  CHECK_ALL(list.Status == ProviderModelListStatus::Success, list.Count == 3);
  CHECK_ALL(StrView_EqCStr(list.Models[0].Id, "gpt-known"),
            StrView_EqCStr(list.Models[0].Name, "Known Model"),
            list.Models[0].ContextWindow == 128000,
            list.Models[0].EffortMask == MODEL_EFFORT_STANDARD_MASK);
  // A prefix match inherits capabilities but keeps its own ID as its name.
  CHECK_ALL(StrView_EqCStr(list.Models[1].Id, "gpt-known-preview"),
            StrView_EqCStr(list.Models[1].Name, "gpt-known-preview"),
            list.Models[1].ContextWindow == 128000);
  // Families the endpoint cannot serve never reach the picker.
  CHECK_ALL(StrView_EqCStr(list.Models[2].Id, "o3-pro"),
            list.Models[2].ContextWindow == 32768,
            list.Models[2].EffortMask == 0);

  ProviderModelList bad = {};
  CHECK(!Responses_ParseModelsResponse(&arena, &endpoint, SVL("[]"), &bad));
  CHECK(bad.Status == ProviderModelListStatus::Failed);
  CHECK(!Responses_ParseModelsResponse(&arena, &endpoint,
                                       SVL("{\"data\":[{}]}"), &bad));
  CHECK(bad.Error.Length != 0);
  ArenaRelease(&arena);
}

}  // namespace

void TestResponses() {
  TestUrls();
  TestRequestBasics();
  TestRequestTools();
  TestRequestMessageShapes();
  TestStreamText();
  TestStreamReasoningAndTools();
  TestStreamEdgeCases();
  TestModelsCatalog();
}
