#include "base/thread.h"
#include "llm/responses.h"
#include "net/http.h"

namespace {

constexpr u32 RESPONSES_HEADER_CAPACITY = 3;

bool OnResponsesData(void* user, StrView chunk) {
  ResponsesParse_Feed((ResponsesParse*)user, chunk);
  return true;
}

bool BuildResponsesHeaders(Arena* arena, ProviderAuth auth, bool streaming,
                           HttpHeader* headers, u32* count, StrView* error) {
  *count = 0;
  *error = {};
  if (!auth.Token.Length) {
    *error = SVL("credentials are unavailable for this provider");
    return false;
  }

  headers[(*count)++] =
      HttpHeader{SVL("content-type"), SVL("application/json")};
  headers[(*count)++] = HttpHeader{
      SVL("accept"),
      streaming ? SVL("text/event-stream") : SVL("application/json")};
  headers[(*count)++] = Http_BearerHeader(arena, auth.Token);
  return true;
}

void EmitSimple(StreamSink sink, StreamEventKind kind) {
  StreamEvent event = {};
  event.Kind = kind;
  StreamSink_Emit(sink, event);
}

bool FailResponses(Arena* arena, const Model* model, StreamSink sink,
                   StrView error, StopReason stop, Message* out) {
  Message message = Message_AssistantEmpty(arena);
  message.Model = model ? model->Id : StrView{};
  message.Provider = model ? model->Provider : StrView{};
  message.Stop = stop;
  message.ErrorMessage = StrView_Dup(
      arena, error.Length ? error : SVL("the model request failed"));
  message.Timestamp = NowMillis();
  EmitSimple(sink, StreamEventKind::Error);
  *out = message;
  return false;
}

bool Responses_Stream(void* ctx, Arena* arena, const Model* model,
                      const LlmContext* context, const StreamOptions* options,
                      StreamSink sink, Message* out) {
  (void)ctx;
  StreamOptions resolved = options ? *options : StreamOptions{};
  if (!Model_RequestProfileIsValid(model, resolved.Profile)) {
    return FailResponses(
        arena, model, sink,
        SVL("the selected model does not support the requested effort or "
            "context window"),
        StopReason::Error, out);
  }
  resolved.Profile = Model_ResolveRequestProfile(model, resolved.Profile);

  if (!model->BaseUrl.Length) {
    return FailResponses(arena, model, sink,
                         SVL("no endpoint is configured for this provider"),
                         StopReason::Error, out);
  }

  HttpHeader headers[RESPONSES_HEADER_CAPACITY];
  u32 headerCount = 0;
  StrView headerError = {};
  if (!BuildResponsesHeaders(arena, resolved.Auth, true, headers, &headerCount,
                             &headerError)) {
    return FailResponses(arena, model, sink, headerError, StopReason::Error,
                         out);
  }

  StrBuf body = Responses_BuildRequest(arena, model, context, &resolved);
  Message message = Message_AssistantEmpty(arena);
  message.Model = model->Id;
  message.Provider = model->Provider;
  ResponsesParse parser = {};
  ResponsesParse_Init(&parser, arena, &message, sink);
  EmitSimple(sink, StreamEventKind::Start);

  HttpRequest request = {};
  request.Method = SVL("POST");
  request.Url = Responses_Url(arena, model->BaseUrl, "responses");
  request.Headers = headers;
  request.HeaderCount = headerCount;
  request.Body = StrBuf_View(&body);
  request.BufferNonSuccessBody = true;
  HttpStreamSink httpSink = {};
  httpSink.User = &parser;
  httpSink.OnData = OnResponsesData;
  HttpResult result =
      resolved.Retry
          ? Http_StreamWithRetry(arena, &request, httpSink, resolved.Abort,
                                 resolved.Retry)
          : Http_Stream(arena, &request, httpSink, resolved.Abort);

  ResponsesParse_Finish(&parser);
  message.Use = parser.Use;
  message.Timestamp = NowMillis();

  if (parser.SawError || result.Status < 0 || result.Status >= 400 ||
      !ResponsesParse_SawTerminal(&parser)) {
    bool aborted = result.Status < 0 && StrView_EqCStr(result.Error, "aborted");
    message.Stop = aborted ? StopReason::Aborted : StopReason::Error;
    StrBuf error = StrBuf_Make(arena, 160);
    if (parser.SawError && parser.Error.Length) {
      StrBuf_Append(&error, parser.Error);
    } else if (result.Status >= 400) {
      StrBuf_AppendCStr(&error, "HTTP ");
      StrBuf_AppendI64(&error, result.Status);
      if (result.Body.Length) {
        StrBuf_AppendCStr(&error, ": ");
        StrBuf_Append(&error, result.Body);
      }
    } else if (result.Error.Length) {
      StrBuf_Append(&error, result.Error);
    } else {
      StrBuf_AppendCStr(&error,
                        "the stream ended without a terminal response");
    }
    message.ErrorMessage = StrView_Dup(arena, StrBuf_View(&error));
    EmitSimple(sink, StreamEventKind::Error);
    *out = message;
    return false;
  }

  message.Stop = parser.Stop;
  EmitSimple(sink, StreamEventKind::Done);
  *out = message;
  return true;
}

HttpResponse ResponsesHttpFetch(void*, Arena* arena, const HttpRequest* request,
                                volatile i32* abort) {
  return Http_Fetch(arena, request, abort);
}

}  // namespace

Provider Responses_Provider() {
  Provider provider = {};
  provider.Stream = Responses_Stream;
  return provider;
}

ProviderModelList Responses_ListModelsWithFetch(
    Arena* arena, const ResponsesEndpoint* endpoint, ProviderAuth auth,
    volatile i32* abort, ResponsesModelsFetchFn fetch, void* fetchUser) {
  if (!endpoint)
    return ProviderModelList_Error(arena, ProviderModelListStatus::Unavailable,
                                   SVL("no endpoint is configured"));
  if (!fetch)
    return ProviderModelList_Error(arena, ProviderModelListStatus::Unavailable,
                                   SVL("the model transport is unavailable"));
  if (!auth.Token.Length)
    return ProviderModelList_Error(arena, ProviderModelListStatus::Unavailable,
                                   SVL("credentials are unavailable"));
  if (abort && Atomic_LoadI32(abort))
    return ProviderModelList_Error(arena, ProviderModelListStatus::Unavailable,
                                   SVL("model discovery was aborted"));

  HttpHeader headers[RESPONSES_HEADER_CAPACITY];
  u32 headerCount = 0;
  StrView headerError = {};
  if (!BuildResponsesHeaders(arena, auth, false, headers, &headerCount,
                             &headerError)) {
    return ProviderModelList_Error(arena, ProviderModelListStatus::Failed,
                                   headerError);
  }

  StrView baseUrl = ResponsesEndpoint_ResolveBaseUrl(arena, endpoint);
  if (!baseUrl.Length)
    return ProviderModelList_Error(arena, ProviderModelListStatus::Unavailable,
                                   SVL("no endpoint is configured"));

  HttpRequest request = {};
  request.Method = SVL("GET");
  request.Url = Responses_Url(arena, baseUrl, "models");
  request.Headers = headers;
  request.HeaderCount = headerCount;

  HttpResponse response = fetch(fetchUser, arena, &request, abort);
  if (response.Status < 0) {
    return ProviderModelList_Error(
        arena, ProviderModelListStatus::Unavailable,
        response.Error.Length ? response.Error
                              : SVL("the models request failed"));
  }
  if (response.Status < 200 || response.Status >= 300) {
    StrBuf error = StrBuf_Make(arena, 80);
    StrBuf_AppendCStr(&error, "the models request failed with HTTP ");
    StrBuf_AppendI64(&error, response.Status);
    return ProviderModelList_Error(arena, ProviderModelListStatus::Failed,
                                   StrBuf_View(&error));
  }

  ProviderModelList parsed = {};
  if (!Responses_ParseModelsResponse(arena, endpoint, response.Body, &parsed))
    return parsed;
  return parsed;
}

ProviderModelList Responses_ListModels(Arena* arena,
                                       const ResponsesEndpoint* endpoint,
                                       ProviderAuth auth, volatile i32* abort) {
  return Responses_ListModelsWithFetch(arena, endpoint, auth, abort,
                                       ResponsesHttpFetch, nullptr);
}
