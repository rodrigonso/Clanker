#include "base/arena.h"
#include "check.h"
#include "crypto/base64.h"
#include "json/json.h"
#include "llm/openai.h"

namespace {

struct OpenAIFetchFixture {
  HttpResponse Response;
  u32 Calls;
  StrView Url;
  StrView Authorization;
  StrView AccountId;
  StrView Originator;
  StrView Beta;
  StrView FedRamp;
  StrView Accept;
  volatile i32* Abort;
};

StrView OpenAIFindHeader(const HttpRequest* request, const char* name) {
  for (u32 i = 0; i < request->HeaderCount; i++) {
    if (StrView_EqCStr(request->Headers[i].Name, name))
      return request->Headers[i].Value;
  }
  return {};
}

HttpResponse OpenAIFetch(void* user, Arena*, const HttpRequest* request,
                         volatile i32* abort) {
  OpenAIFetchFixture* fixture = (OpenAIFetchFixture*)user;
  fixture->Calls++;
  fixture->Url = request->Url;
  fixture->Authorization = OpenAIFindHeader(request, "authorization");
  fixture->AccountId = OpenAIFindHeader(request, "chatgpt-account-id");
  fixture->Originator = OpenAIFindHeader(request, "originator");
  fixture->Beta = OpenAIFindHeader(request, "openai-beta");
  fixture->FedRamp = OpenAIFindHeader(request, "x-openai-fedramp");
  fixture->Accept = OpenAIFindHeader(request, "accept");
  fixture->Abort = abort;
  return fixture->Response;
}

StrView OpenAITestJwt(Arena* arena, StrView accountId) {
  StrBuf payload = StrBuf_Make(arena, accountId.Length + 80);
  StrBuf_AppendCStr(
      &payload,
      "{\"https://api.openai.com/auth\":{\"chatgpt_account_id\":");
  Json_WriteString(&payload, accountId);
  StrBuf_AppendCStr(&payload, "}}");
  StrView encoded = Base64UrlEncode(
      arena, (const byte*)payload.Data, payload.Length);
  StrBuf jwt = StrBuf_Make(arena, encoded.Length + 8);
  StrBuf_AppendCStr(&jwt, "h.");
  StrBuf_Append(&jwt, encoded);
  StrBuf_AppendCStr(&jwt, ".s");
  return StrBuf_View(&jwt);
}

void TestOpenAIModelMetadata() {
  Model sol = OpenAI_Model(SVL("gpt-5.6-sol"));
  CHECK_ALL(
      StrView_EqCStr(sol.Provider, "openai"),
      StrView_EqCStr(sol.Api, "openai-responses"),
      sol.ContextWindow == 272000, sol.Reasoning,
      Model_DefaultEffort(&sol) == ModelEffort::Low,
      Model_SupportsEffort(&sol, ModelEffort::Ultra));

  Model luna = OpenAI_Model(SVL("gpt-5.6-luna"));
  CHECK_ALL(Model_SupportsEffort(&luna, ModelEffort::Max),
            !Model_SupportsEffort(&luna, ModelEffort::Ultra));

  Model unknown = OpenAI_Model(SVL("future-openai-model"));
  CHECK_ALL(unknown.ContextWindow == 0, unknown.EffortMask == 0,
            StrView_Eq(unknown.Name, unknown.Id));
}

void TestOpenAIRequest() {
  Arena arena = ArenaCreate();
  Model model = OpenAI_Model(SVL("gpt-5.6-sol"));

  Message messages[4];
  messages[0] = Message_User(&arena, SVL("inspect this"));
  Array_Push(&messages[0].Content,
             Content_Image(SVL("AQID"), SVL("image/png")));

  messages[1] = Message_AssistantEmpty(&arena);
  messages[1].Model = model.Id;
  messages[1].Provider = model.Provider;
  Array_Push(
      &messages[1].Content,
      Content_Thinking(
          SVL("summary"),
          SVL("{\"type\":\"reasoning\",\"id\":\"rs_1\",\"summary\":[{"
              "\"type\":\"summary_text\",\"text\":\"summary\"}],"
              "\"encrypted_content\":\"cipher\"}"),
          false));
  Content answer = Content_Text(SVL("working"));
  answer.Signature = SVL("{\"id\":\"msg_1\",\"phase\":\"commentary\"}");
  Array_Push(&messages[1].Content, answer);
  Content call =
      Content_ToolCall(SVL("call_1"), SVL("read"), SVL("{\"path\":\"x\"}"));
  call.Signature = SVL("fc_1");
  Array_Push(&messages[1].Content, call);
  messages[1].Stop = StopReason::ToolUse;

  messages[2] =
      Message_ToolResult(&arena, SVL("call_1"), SVL("read"),
                         SVL("file contents"), false);
  messages[3] = Message_User(&arena, SVL("continue"));

  ToolDef tool = {SVL("read"), SVL("Read a file"),
                  SVL("{\"type\":\"object\",\"properties\":{\"path\":{"
                      "\"type\":\"string\"}},\"required\":[\"path\"]}")};
  LlmContext context = {};
  context.SystemPrompt = SVL("You are Clank.");
  context.Messages = messages;
  context.MessageCount = 4;
  context.Tools = &tool;
  context.ToolCount = 1;
  StreamOptions options = {};
  options.MaxTokens = 4096;
  options.Profile.Effort = ModelEffort::Ultra;
  options.Profile.ContextWindow = model.ContextWindow;

  StrBuf body = OpenAI_BuildRequest(&arena, &model, &context, &options);
  JsonValue* root = Json_Parse(&arena, StrBuf_View(&body));
  CHECK(root != nullptr);
  CHECK_ALL(StrView_EqCStr(Json_GetStr(root, "model"), "gpt-5.6-sol"),
            Json_GetBool(root, "store", true) == false,
            Json_GetBool(root, "stream", false),
            Json_GetInt(root, "max_output_tokens", 0) == 4096,
            StrView_EqCStr(Json_GetStr(root, "instructions"),
                           "You are Clank."));
  CHECK(StrView_EqCStr(
      Json_GetStr(Json_Get(root, "reasoning"), "effort"), "max"));

  JsonValue* tools = Json_Get(root, "tools");
  CHECK(Json_IsArray(tools) && tools->Count == 1);
  CHECK_ALL(
      StrView_EqCStr(Json_GetStr(tools->Items[0], "type"), "function"),
      Json_GetBool(tools->Items[0], "strict", true) == false,
      Json_IsObject(Json_Get(tools->Items[0], "parameters")));

  JsonValue* input = Json_Get(root, "input");
  CHECK(Json_IsArray(input) && input->Count == 6);
  JsonValue* userContent = Json_Get(input->Items[0], "content");
  CHECK(Json_IsArray(userContent) && userContent->Count == 2);
  CHECK_ALL(
      StrView_EqCStr(Json_GetStr(userContent->Items[1], "type"),
                     "input_image"),
      StrView_EqCStr(Json_GetStr(userContent->Items[1], "image_url"),
                     "data:image/png;base64,AQID"));
  CHECK_ALL(
      StrView_EqCStr(Json_GetStr(input->Items[1], "type"), "reasoning"),
      StrView_EqCStr(Json_GetStr(input->Items[1], "encrypted_content"),
                     "cipher"),
      StrView_EqCStr(Json_GetStr(input->Items[2], "id"), "msg_1"),
      StrView_EqCStr(Json_GetStr(input->Items[3], "id"), "fc_1"),
      StrView_EqCStr(Json_GetStr(input->Items[3], "call_id"), "call_1"),
      StrView_EqCStr(Json_GetStr(input->Items[3], "arguments"),
                     "{\"path\":\"x\"}"),
      StrView_EqCStr(Json_GetStr(input->Items[4], "type"),
                     "function_call_output"),
      StrView_EqCStr(Json_GetStr(input->Items[4], "output"),
                     "file contents"));

  messages[1].Model = SVL("different-model");
  StrBuf incompatible =
      OpenAI_BuildRequest(&arena, &model, &context, &options);
  JsonValue* incompatibleRoot =
      Json_Parse(&arena, StrBuf_View(&incompatible));
  JsonValue* incompatibleInput = Json_Get(incompatibleRoot, "input");
  CHECK(Json_IsArray(incompatibleInput) && incompatibleInput->Count == 5);
  CHECK_ALL(
      StrView_EqCStr(Json_GetStr(incompatibleInput->Items[1], "type"),
                     "message"),
      !StrView_EqCStr(Json_GetStr(incompatibleInput->Items[1], "id"),
                      "msg_1"),
      Json_Get(incompatibleInput->Items[2], "id") == nullptr);

  ArenaRelease(&arena);
}

void TestOpenAIStream() {
  const char* fixture =
      "data: {\"type\":\"response.output_item.added\",\"output_index\":0,"
      "\"item\":{\"type\":\"reasoning\",\"id\":\"rs_1\"}}\n\n"
      "data: {\"type\":\"response.reasoning_summary_text.delta\","
      "\"output_index\":0,\"delta\":\"Checking\"}\n\n"
      "data: {\"type\":\"response.output_item.done\",\"output_index\":0,"
      "\"item\":{\"type\":\"reasoning\",\"id\":\"rs_1\","
      "\"summary\":[{\"type\":\"summary_text\",\"text\":\"Checking\"}],"
      "\"encrypted_content\":\"secret\"}}\n\n"
      "data: {\"type\":\"response.output_item.added\",\"output_index\":1,"
      "\"item\":{\"type\":\"message\",\"id\":\"msg_1\","
      "\"phase\":\"final_answer\",\"content\":[]}}\n\n"
      "data: {\"type\":\"response.output_text.delta\",\"output_index\":1,"
      "\"delta\":\"Hello\"}\n\n"
      "data: {\"type\":\"response.output_item.done\",\"output_index\":1,"
      "\"item\":{\"type\":\"message\",\"id\":\"msg_1\","
      "\"phase\":\"final_answer\",\"content\":[{\"type\":\"output_text\","
      "\"text\":\"Hello\",\"annotations\":[]}]}}\n\n"
      "data: {\"type\":\"response.output_item.added\",\"output_index\":2,"
      "\"item\":{\"type\":\"function_call\",\"id\":\"fc_1\","
      "\"call_id\":\"call_1\",\"name\":\"read\",\"arguments\":\"\"}}\n\n"
      "data: {\"type\":\"response.function_call_arguments.delta\","
      "\"output_index\":2,\"delta\":\"{\\\"path\\\":\"}\n\n"
      "data: {\"type\":\"response.function_call_arguments.delta\","
      "\"output_index\":2,\"delta\":\"\\\"x\\\"}\"}\n\n"
      "data: {\"type\":\"response.output_item.done\",\"output_index\":2,"
      "\"item\":{\"type\":\"function_call\",\"id\":\"fc_1\","
      "\"call_id\":\"call_1\",\"name\":\"read\","
      "\"arguments\":\"{\\\"path\\\":\\\"x\\\"}\"}}\n\n"
      "data: {\"type\":\"response.completed\",\"response\":{\"status\":"
      "\"completed\",\"usage\":{\"input_tokens\":70,\"output_tokens\":30,"
      "\"total_tokens\":100,\"input_tokens_details\":{\"cached_tokens\":20,"
      "\"cache_write_tokens\":5},\"output_tokens_details\":{"
      "\"reasoning_tokens\":12}}}}\n\n"
      "data: [DONE]\n\n";

  Arena arena = ArenaCreate();
  Message message = Message_AssistantEmpty(&arena);
  OpenAIParse parser = {};
  OpenAIParse_Init(&parser, &arena, &message, StreamSink{});
  StrView all = SV(fixture);
  for (u32 offset = 0; offset < all.Length; offset += 7) {
    u32 count = ClankMin<u32>(7, all.Length - offset);
    OpenAIParse_Feed(&parser, StrView{all.Data + offset, count});
  }
  message.Use = parser.Use;
  message.Stop = parser.Stop;

  CHECK_ALL(message.Content.Count == 3,
            message.Content.Data[0].Kind == ContentKind::Thinking,
            StrView_EqCStr(message.Content.Data[0].Text, "Checking"),
            StrView_Contains(message.Content.Data[0].Signature,
                             "\"encrypted_content\":\"secret\""),
            message.Content.Data[1].Kind == ContentKind::Text,
            StrView_EqCStr(message.Content.Data[1].Text, "Hello"),
            StrView_Contains(message.Content.Data[1].Signature,
                             "\"id\":\"msg_1\""),
            message.Content.Data[2].Kind == ContentKind::ToolCall,
            StrView_EqCStr(message.Content.Data[2].ToolId, "call_1"),
            StrView_EqCStr(message.Content.Data[2].Signature, "fc_1"),
            StrView_EqCStr(message.Content.Data[2].ToolArgsJson,
                           "{\"path\":\"x\"}"));
  CHECK_ALL(message.Stop == StopReason::ToolUse, message.Use.Input == 45,
            message.Use.CacheRead == 20, message.Use.CacheWrite == 5,
            message.Use.Output == 30, message.Use.Reasoning == 12,
            message.Use.TotalTokens == 100, parser.SawTerminal,
            !parser.SawError);

  Message failed = Message_AssistantEmpty(&arena);
  OpenAIParse failureParser = {};
  OpenAIParse_Init(&failureParser, &arena, &failed, StreamSink{});
  OpenAIParse_Feed(
      &failureParser,
      SVL("data: {\"type\":\"response.failed\",\"response\":{\"error\":{"
          "\"message\":\"quota exhausted\"}}}\n\n"));
  CHECK_ALL(failureParser.SawError,
            failureParser.Stop == StopReason::Error,
            StrView_EqCStr(failureParser.Error, "quota exhausted"));

  Message truncated = Message_AssistantEmpty(&arena);
  OpenAIParse truncatedParser = {};
  OpenAIParse_Init(&truncatedParser, &arena, &truncated, StreamSink{});
  OpenAIParse_Feed(
      &truncatedParser,
      SVL("data: {\"type\":\"response.incomplete\",\"response\":{"
          "\"incomplete_details\":{\"reason\":\"max_output_tokens\"}}}\n\n"));
  CHECK_ALL(!truncatedParser.SawError,
            truncatedParser.Stop == StopReason::Length);

  Message filtered = Message_AssistantEmpty(&arena);
  OpenAIParse filteredParser = {};
  OpenAIParse_Init(&filteredParser, &arena, &filtered, StreamSink{});
  OpenAIParse_Feed(
      &filteredParser,
      SVL("data: {\"type\":\"response.incomplete\",\"response\":{"
          "\"incomplete_details\":{\"reason\":\"content_filter\"}}}\n\n"));
  CHECK_ALL(filteredParser.SawError,
            filteredParser.Stop == StopReason::Error,
            StrView_EqCStr(filteredParser.Error, "content_filter"));
  ArenaRelease(&arena);
}

void TestOpenAIModelCatalogs() {
  Arena arena = ArenaCreate();
  ProviderModelList publicModels = {};
  CHECK(OpenAI_ParseModelsResponse(
      &arena,
      SVL("{\"data\":[{\"id\":\"gpt-5.6-sol\"},"
          "{\"id\":\"text-embedding-3-large\"},{\"id\":\"custom\"}]}"),
      false, &publicModels));
  CHECK_ALL(publicModels.Status == ProviderModelListStatus::Success,
            publicModels.Count == 1,
            StrView_EqCStr(publicModels.Models[0].Id, "gpt-5.6-sol"),
            publicModels.Models[0].ContextWindow == 272000,
            Model_SupportsEffort(&publicModels.Models[0],
                                 ModelEffort::Ultra));

  ProviderModelList richModels = {};
  CHECK(OpenAI_ParseModelsResponse(
      &arena,
      SVL("{\"models\":[{\"slug\":\"gpt-live\","
          "\"display_name\":\"GPT Live\",\"visibility\":\"list\","
          "\"supported_in_api\":true,\"context_window\":400000,"
          "\"default_reasoning_level\":\"high\","
          "\"supported_reasoning_levels\":[{\"effort\":\"low\"},"
          "{\"effort\":\"high\"},{\"effort\":\"ultra\"}]},"
          "{\"slug\":\"hidden\",\"visibility\":\"hide\","
          "\"supported_in_api\":true}]}"),
      true, &richModels));
  CHECK_ALL(richModels.Count == 1,
            StrView_EqCStr(richModels.Models[0].Name, "GPT Live"),
            richModels.Models[0].ContextWindow == 400000,
            Model_DefaultEffort(&richModels.Models[0]) ==
                ModelEffort::High,
            Model_SupportsEffort(&richModels.Models[0],
                                 ModelEffort::Ultra));

  ProviderModelList malformed = {};
  CHECK(!OpenAI_ParseModelsResponse(&arena, SVL("{}"), false, &malformed));
  CHECK_ALL(malformed.Status == ProviderModelListStatus::Failed,
            malformed.Error.Length != 0);
  ArenaRelease(&arena);
}

void TestOpenAIModelDiscovery() {
  Arena arena = ArenaCreate();
  OpenAIFetchFixture api = {};
  api.Response.Status = 200;
  api.Response.Body =
      SVL("{\"data\":[{\"id\":\"gpt-5.6-sol\"},"
          "{\"id\":\"text-embedding-3-large\"}]}");
  volatile i32 abort = 0;
  ProviderModelList apiModels = OpenAI_ListModelsWithFetch(
      &arena, ProviderAuth{ProviderAuthKind::ApiKey, SVL("sk-openai")},
      &abort, OpenAIFetch, &api);
  CHECK_ALL(
      apiModels.Status == ProviderModelListStatus::Success,
      apiModels.Count == 1, api.Calls == 1, api.Abort == &abort,
      StrView_EqCStr(api.Url, "https://api.openai.com/v1/models"),
      api.Authorization.Length > SVL("sk-openai").Length,
      api.AccountId.Length == 0, api.Originator.Length == 0,
      StrView_EqCStr(api.Accept, "application/json"));
  CHECK_ALL(
      apiModels.Models[0].ContextWindow == 272000,
      Model_SupportsEffort(&apiModels.Models[0], ModelEffort::Ultra));

  OpenAIFetchFixture oauth = {};
  oauth.Response.Status = 200;
  oauth.Response.Body =
      SVL("{\"models\":[{\"slug\":\"gpt-oauth\","
          "\"display_name\":\"GPT OAuth\",\"visibility\":\"list\","
          "\"supported_in_api\":true,\"context_window\":300000,"
          "\"default_reasoning_level\":\"high\","
          "\"supported_reasoning_levels\":[{\"effort\":\"low\"},"
          "{\"effort\":\"high\"}]}]}");
  ProviderModelList oauthModels = OpenAI_ListModelsWithFetch(
      &arena,
      ProviderAuth{ProviderAuthKind::OAuth, SVL("opaque-access"),
                   SVL("acct_123"), true},
      nullptr, OpenAIFetch, &oauth);
  CHECK_ALL(
      oauthModels.Status == ProviderModelListStatus::Success,
      oauthModels.Count == 1, oauth.Calls == 1,
      StrView_EqCStr(
          oauth.Url,
          "https://chatgpt.com/backend-api/codex/models?"
          "client_version=0.146.0"),
      StrView_EqCStr(oauth.AccountId, "acct_123"),
      StrView_EqCStr(oauth.Originator, "clank"),
      StrView_EqCStr(oauth.Beta, "responses=experimental"),
      StrView_EqCStr(oauth.FedRamp, "true"),
      oauth.Authorization.Length > 16);

  ProviderModelList missing =
      OpenAI_ListModelsWithFetch(&arena, {}, nullptr, OpenAIFetch, &api);
  CHECK(missing.Status == ProviderModelListStatus::Unavailable);
  ProviderModelList invalidOAuth = OpenAI_ListModelsWithFetch(
      &arena, ProviderAuth{ProviderAuthKind::OAuth, SVL("not-a-jwt")},
      nullptr, OpenAIFetch, &oauth);
  CHECK_ALL(invalidOAuth.Status == ProviderModelListStatus::Failed,
            StrView_Contains(invalidOAuth.Error, "account ID"));

  OpenAIFetchFixture transport = {};
  transport.Response.Status = -1;
  transport.Response.Error = SVL("offline");
  ProviderModelList unavailable = OpenAI_ListModelsWithFetch(
      &arena, ProviderAuth{ProviderAuthKind::ApiKey, SVL("token")},
      nullptr, OpenAIFetch, &transport);
  CHECK_ALL(unavailable.Status == ProviderModelListStatus::Unavailable,
            StrView_EqCStr(unavailable.Error, "offline"));

  OpenAIFetchFixture unauthorized = {};
  unauthorized.Response.Status = 401;
  ProviderModelList failed = OpenAI_ListModelsWithFetch(
      &arena, ProviderAuth{ProviderAuthKind::ApiKey, SVL("token")},
      nullptr, OpenAIFetch, &unauthorized);
  CHECK(failed.Status == ProviderModelListStatus::Failed);

  OpenAIFetchFixture malformed = {};
  malformed.Response.Status = 200;
  malformed.Response.Body = SVL("{}");
  ProviderModelList bad = OpenAI_ListModelsWithFetch(
      &arena, ProviderAuth{ProviderAuthKind::ApiKey, SVL("token")},
      nullptr, OpenAIFetch, &malformed);
  CHECK(bad.Status == ProviderModelListStatus::Failed);

  abort = 1;
  OpenAIFetchFixture unused = {};
  ProviderModelList aborted = OpenAI_ListModelsWithFetch(
      &arena, ProviderAuth{ProviderAuthKind::ApiKey, SVL("token")},
      &abort, OpenAIFetch, &unused);
  CHECK_ALL(aborted.Status == ProviderModelListStatus::Unavailable,
            unused.Calls == 0);
  ArenaRelease(&arena);
}

}  // namespace

void TestOpenAI() {
  TestOpenAIModelMetadata();
  TestOpenAIRequest();
  TestOpenAIStream();
  TestOpenAIModelCatalogs();
  TestOpenAIModelDiscovery();
}
