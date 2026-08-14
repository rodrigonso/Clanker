#include "auth/oauth.h"
#include "base/thread.h"
#include "llm/openai.h"
#include "net/http.h"

namespace {

constexpr u32 OPENAI_HEADER_CAPACITY = 8;

bool OnOpenAIData(void* user, StrView chunk) {
  OpenAIParse_Feed((OpenAIParse*)user, chunk);
  return true;
}

bool BuildOpenAIHeaders(Arena* arena, ProviderAuth auth, bool streaming,
                        HttpHeader* headers, u32* count, StrView* error) {
  *count = 0;
  *error = {};
  if (!auth.Token.Length) {
    *error = SVL("OpenAI credentials are unavailable");
    return false;
  }

  headers[(*count)++] =
      HttpHeader{SVL("content-type"), SVL("application/json")};
  headers[(*count)++] =
      HttpHeader{SVL("accept"), streaming ? SVL("text/event-stream")
                                         : SVL("application/json")};

  headers[(*count)++] = Http_BearerHeader(arena, auth.Token);

  if (auth.Kind == ProviderAuthKind::OAuth) {
    StrView accountId = auth.AccountId;
    if (!accountId.Length)
      OAuth_ParseChatGptAccountId(arena, auth.Token, &accountId);
    if (!accountId.Length) {
      *error = SVL("OpenAI OAuth token has no ChatGPT account ID");
      return false;
    }
    headers[(*count)++] =
        HttpHeader{SVL("chatgpt-account-id"), accountId};
    headers[(*count)++] = HttpHeader{SVL("originator"), SVL("clank")};
    headers[(*count)++] =
        HttpHeader{SVL("user-agent"), SVL("clank-coding-agent")};
    headers[(*count)++] = HttpHeader{SVL("openai-beta"),
                                    SVL("responses=experimental")};
    if (auth.IsFedRamp)
      headers[(*count)++] =
          HttpHeader{SVL("x-openai-fedramp"), SVL("true")};
  }
  return true;
}

StrView OpenAIResponsesUrl(Arena* arena, const Model* model,
                           ProviderAuthKind kind) {
  if (kind == ProviderAuthKind::OAuth)
    return SVL("https://chatgpt.com/backend-api/codex/responses");
  StrBuf url = StrBuf_Make(arena, model->BaseUrl.Length + 24);
  StrBuf_Append(&url, model->BaseUrl);
  StrBuf_AppendCStr(&url, "/v1/responses");
  return StrBuf_View(&url);
}

StrView OpenAIModelsUrl(ProviderAuthKind kind) {
  return kind == ProviderAuthKind::OAuth
             ? SVL("https://chatgpt.com/backend-api/codex/models?"
                   "client_version=0.146.0")
             : SVL("https://api.openai.com/v1/models");
}

void EmitSimple(StreamSink sink, StreamEventKind kind) {
  StreamEvent event = {};
  event.Kind = kind;
  StreamSink_Emit(sink, event);
}

bool FailOpenAI(Arena* arena, const Model* model, StreamSink sink,
                StrView error, StopReason stop, Message* out) {
  Message message = Message_AssistantEmpty(arena);
  message.Model = model ? model->Id : StrView{};
  message.Provider = model ? model->Provider : StrView{};
  message.Stop = stop;
  message.ErrorMessage =
      StrView_Dup(arena, error.Length ? error : SVL("OpenAI request failed"));
  message.Timestamp = NowMillis();
  EmitSimple(sink, StreamEventKind::Error);
  *out = message;
  return false;
}

bool OpenAI_Stream(void*, Arena* arena, const Model* model,
                   const LlmContext* context, const StreamOptions* options,
                   StreamSink sink, Message* out) {
  StreamOptions resolved = options ? *options : StreamOptions{};
  if (!Model_RequestProfileIsValid(model, resolved.Profile)) {
    return FailOpenAI(
        arena, model, sink,
        SVL("the selected model does not support the requested effort or "
            "context window"),
        StopReason::Error, out);
  }
  resolved.Profile = Model_ResolveRequestProfile(model, resolved.Profile);

  HttpHeader headers[OPENAI_HEADER_CAPACITY];
  u32 headerCount = 0;
  StrView headerError = {};
  if (!BuildOpenAIHeaders(arena, resolved.Auth, true, headers,
                          &headerCount, &headerError)) {
    return FailOpenAI(arena, model, sink, headerError, StopReason::Error,
                      out);
  }

  StrBuf body = OpenAI_BuildRequest(arena, model, context, &resolved);
  Message message = Message_AssistantEmpty(arena);
  message.Model = model->Id;
  message.Provider = model->Provider;
  OpenAIParse parser = {};
  OpenAIParse_Init(&parser, arena, &message, sink);
  EmitSimple(sink, StreamEventKind::Start);

  HttpRequest request = {};
  request.Method = SVL("POST");
  request.Url = OpenAIResponsesUrl(arena, model, resolved.Auth.Kind);
  request.Headers = headers;
  request.HeaderCount = headerCount;
  request.Body = StrBuf_View(&body);
  request.BufferNonSuccessBody = true;
  HttpStreamSink httpSink = {};
  httpSink.User = &parser;
  httpSink.OnData = OnOpenAIData;
  HttpResult result =
      resolved.Retry
          ? Http_StreamWithRetry(arena, &request, httpSink, resolved.Abort,
                                 resolved.Retry)
          : Http_Stream(arena, &request, httpSink, resolved.Abort);

  message.Use = parser.Use;
  message.Timestamp = NowMillis();
  if (parser.SawError || result.Status < 0 || result.Status >= 400 ||
      !parser.SawTerminal) {
    bool aborted =
        result.Status < 0 && StrView_EqCStr(result.Error, "aborted");
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
                       "OpenAI stream ended without a terminal response");
    }
    message.ErrorMessage =
        StrView_Dup(arena, StrBuf_View(&error));
    EmitSimple(sink, StreamEventKind::Error);
    *out = message;
    return false;
  }

  message.Stop = parser.Stop;
  EmitSimple(sink, StreamEventKind::Done);
  *out = message;
  return true;
}

HttpResponse OpenAIHttpFetch(void*, Arena* arena,
                             const HttpRequest* request,
                             volatile i32* abort) {
  return Http_Fetch(arena, request, abort);
}

}  // namespace

Provider OpenAI_Provider() {
  Provider provider = {};
  provider.Stream = OpenAI_Stream;
  return provider;
}

ProviderModelList OpenAI_ListModelsWithFetch(
    Arena* arena, ProviderAuth auth, volatile i32* abort,
    OpenAIModelsFetchFn fetch, void* fetchUser) {
  if (!auth.Token.Length)
    return ProviderModelList_Error(arena, ProviderModelListStatus::Unavailable,
                          SVL("OpenAI credentials are unavailable"));
  if (!fetch)
    return ProviderModelList_Error(arena, ProviderModelListStatus::Unavailable,
                          SVL("OpenAI model transport is unavailable"));
  if (abort && Atomic_LoadI32(abort))
    return ProviderModelList_Error(arena, ProviderModelListStatus::Unavailable,
                          SVL("OpenAI model discovery was aborted"));

  HttpHeader headers[OPENAI_HEADER_CAPACITY];
  u32 headerCount = 0;
  StrView headerError = {};
  if (!BuildOpenAIHeaders(arena, auth, false, headers, &headerCount,
                          &headerError)) {
    return ProviderModelList_Error(arena, ProviderModelListStatus::Failed,
                          headerError);
  }
  HttpRequest request = {};
  request.Method = SVL("GET");
  request.Url = OpenAIModelsUrl(auth.Kind);
  request.Headers = headers;
  request.HeaderCount = headerCount;

  HttpResponse response = fetch(fetchUser, arena, &request, abort);
  if (response.Status < 0) {
    return ProviderModelList_Error(
        arena, ProviderModelListStatus::Unavailable,
        response.Error.Length ? response.Error
                              : SVL("OpenAI model request failed"));
  }
  if (response.Status < 200 || response.Status >= 300) {
    StrBuf error = StrBuf_Make(arena, 80);
    StrBuf_AppendCStr(&error,
                     "OpenAI models request failed with HTTP ");
    StrBuf_AppendI64(&error, response.Status);
    return ProviderModelList_Error(arena, ProviderModelListStatus::Failed,
                          StrBuf_View(&error));
  }

  ProviderModelList parsed = {};
  if (!OpenAI_ParseModelsResponse(arena, response.Body,
                                  auth.Kind == ProviderAuthKind::OAuth,
                                  &parsed))
    return parsed;
  return ProviderModelList_Merge(arena, parsed, nullptr, 0);
}

ProviderModelList OpenAI_ListModels(Arena* arena, ProviderAuth auth,
                                    volatile i32* abort) {
  return OpenAI_ListModelsWithFetch(arena, auth, abort, OpenAIHttpFetch,
                                    nullptr);
}
