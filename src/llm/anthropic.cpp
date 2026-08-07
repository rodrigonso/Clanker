#include "base/str.h"
#include "base/thread.h"
#include "llm/anthropic.h"
#include "net/http.h"

// Anthropic Messages provider: HTTP glue around the protocol module.

constexpr u32 ANTHROPIC_HEADER_CAPACITY = 6;

static bool OnHttpData(void* user, StrView chunk) {
  AnthropicParse_Feed((AnthropicParse*)user, chunk);
  return true;
}

static u32 Anthropic_BuildHeaders(Arena* arena, ProviderAuth auth,
                                  const Model* model,
                                  const StreamOptions* options,
                                  HttpHeader* headers) {
  u32 count = 0;
  headers[count++] = HttpHeader{SVL("anthropic-version"), SVL("2023-06-01")};
  headers[count++] = HttpHeader{SVL("content-type"), SVL("application/json")};
  StrView contextBeta = Anthropic_ContextBeta(model, options);
  bool oauth = Anthropic_UsesOAuth(auth);
  if (oauth) {
    StrBuf bearer = StrBuf_Make(arena, auth.Token.Length + 8);
    StrBuf_AppendCStr(&bearer, "Bearer ");
    StrBuf_Append(&bearer, auth.Token);
    headers[count++] = HttpHeader{SVL("authorization"), StrBuf_View(&bearer)};
    headers[count++] = HttpHeader{SVL("user-agent"), SVL("claude-cli/2.1.75")};
    headers[count++] = HttpHeader{SVL("x-app"), SVL("cli")};
  } else {
    headers[count++] = HttpHeader{SVL("x-api-key"), auth.Token};
  }
  if (oauth || contextBeta.Length) {
    StrView oauthBetas = SVL("claude-code-20250219,oauth-2025-04-20");
    StrBuf betas =
        StrBuf_Make(arena, oauthBetas.Length + contextBeta.Length + 2);
    if (oauth) StrBuf_Append(&betas, oauthBetas);
    if (contextBeta.Length) {
      if (betas.Length) StrBuf_AppendChar(&betas, ',');
      StrBuf_Append(&betas, contextBeta);
    }
    headers[count++] =
        HttpHeader{SVL("anthropic-beta"), StrBuf_View(&betas)};
  }
  return count;
}

static bool Anthropic_Stream(void* ctx, Arena* arena, const Model* model, const LlmContext* context,
                             const StreamOptions* options, StreamSink sink, Message* out) {
  (void)ctx;
  StreamOptions resolved = options ? *options : StreamOptions{};
  if (!Model_RequestProfileIsValid(model, resolved.Profile)) {
    Message invalid = Message_AssistantEmpty(arena);
    invalid.Model = model ? model->Id : StrView{};
    invalid.Provider = model ? model->Provider : StrView{};
    invalid.Stop = StopReason::Error;
    invalid.ErrorMessage =
        SVL("the selected model does not support the requested effort or context window");
    StreamEvent error = {};
    error.Kind = StreamEventKind::Error;
    StreamSink_Emit(sink, error);
    *out = invalid;
    return false;
  }
  resolved.Profile =
      Model_ResolveRequestProfile(model, resolved.Profile);
  StrBuf body = Anthropic_BuildRequest(arena, model, context, &resolved);

  StrBuf url = StrBuf_Make(arena, model->BaseUrl.Length + 16);
  StrBuf_Append(&url, model->BaseUrl);
  StrBuf_AppendCStr(&url, "/v1/messages");

  HttpHeader headers[ANTHROPIC_HEADER_CAPACITY];
  u32 hc =
      Anthropic_BuildHeaders(arena, resolved.Auth, model, &resolved, headers);

  Message m = Message_AssistantEmpty(arena);
  m.Model = model->Id;
  m.Provider = model->Provider;

  AnthropicParse st;
  AnthropicParse_Init(&st, arena, &m, sink);

  StreamEvent start = {};
  start.Kind = StreamEventKind::Start;
  StreamSink_Emit(sink, start);

  HttpRequest req = {};
  req.Method = SVL("POST");
  req.Url = StrBuf_View(&url);
  req.Headers = headers;
  req.HeaderCount = hc;
  req.Body = StrBuf_View(&body);
  // A non-2xx response body is JSON error text, not SSE; never let it reach
  // the SSE parser (see AnthropicParse_Feed) -- use HttpResult::Body instead.
  req.BufferNonSuccessBody = true;

  HttpStreamSink hsink = {};
  hsink.User = &st;
  hsink.OnData = OnHttpData;

  HttpResult r = resolved.Retry ? Http_StreamWithRetry(arena, &req, hsink, resolved.Abort, resolved.Retry)
                                : Http_Stream(arena, &req, hsink, resolved.Abort);

  m.Use = st.Use;
  m.Timestamp = NowMillis();

  if (st.SawError || r.Status < 0 || r.Status >= 400) {
    bool aborted = (r.Status < 0 && StrView_EqCStr(r.Error, "aborted"));
    m.Stop = aborted ? StopReason::Aborted : StopReason::Error;
    StrBuf e = StrBuf_Make(arena, 128);
    if (st.SawError && st.ErrMsg.Length) {
      StrBuf_Append(&e, st.ErrMsg);
    } else if (r.Status >= 400) {
      StrBuf_AppendCStr(&e, "HTTP ");
      StrBuf_AppendU64(&e, (u64)r.Status);
      if (r.Body.Length) {
        StrBuf_AppendCStr(&e, ": ");
        StrBuf_Append(&e, r.Body);
      }
    } else {
      StrBuf_Append(&e, r.Error);
    }
    m.ErrorMessage = StrView_Dup(arena, StrBuf_View(&e));
    StreamEvent ee = {};
    ee.Kind = StreamEventKind::Error;
    StreamSink_Emit(sink, ee);
    *out = m;
    return false;
  }

  m.Stop = st.Stop;
  StreamEvent done = {};
  done.Kind = StreamEventKind::Done;
  StreamSink_Emit(sink, done);
  *out = m;
  return true;
}

static ProviderModelList AnthropicModelListError(Arena* arena, ProviderModelListStatus status,
                                                 StrView error) {
  ProviderModelList result = {};
  result.Status = status;
  result.Error = StrView_Dup(arena, error);
  return result;
}

static void AppendQueryValue(StrBuf* url, StrView value) {
  static const char HEX[] = "0123456789ABCDEF";
  for (u32 i = 0; i < value.Length; i++) {
    u8 c = (u8)value.Data[i];
    bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
    if (plain) {
      StrBuf_AppendChar(url, (char)c);
    } else {
      StrBuf_AppendChar(url, '%');
      StrBuf_AppendChar(url, HEX[c >> 4]);
      StrBuf_AppendChar(url, HEX[c & 15]);
    }
  }
}

static StrView HttpStatusError(Arena* arena, i32 status) {
  StrBuf error = StrBuf_Make(arena, 64);
  StrBuf_AppendCStr(&error, "Anthropic models request failed with HTTP ");
  StrBuf_AppendU64(&error, (u64)status);
  return StrBuf_View(&error);
}

static HttpResponse AnthropicHttpFetch(void* user, Arena* arena, const HttpRequest* request,
                                      volatile i32* abort) {
  (void)user;
  return Http_Fetch(arena, request, abort);
}

ProviderModelList Anthropic_ListModelsWithFetch(Arena* arena,
                                                ProviderAuth auth,
                                                volatile i32* abort,
                                                AnthropicModelsFetchFn fetch, void* fetchUser) {
  if (auth.Token.Length == 0) {
    return AnthropicModelListError(arena, ProviderModelListStatus::Unavailable,
                                   SVL("Anthropic credentials are unavailable"));
  }
  if (!fetch) {
    return AnthropicModelListError(arena, ProviderModelListStatus::Unavailable,
                                   SVL("Anthropic model transport is unavailable"));
  }
  if (abort && Atomic_LoadI32(abort)) {
    return AnthropicModelListError(arena, ProviderModelListStatus::Unavailable,
                                   SVL("Anthropic model discovery was aborted"));
  }

  Array<Model> models = Array_Make<Model>(arena, 16);
  Array<StrView> cursors = Array_Make<StrView>(arena, 4);
  StrView afterId = {};

  for (u32 pageNumber = 0; pageNumber < 100; pageNumber++) {
    StrBuf url = StrBuf_Make(arena, 96 + afterId.Length * 3);
    StrBuf_AppendCStr(&url, "https://api.anthropic.com/v1/models?limit=1000");
    if (afterId.Length) {
      StrBuf_AppendCStr(&url, "&after_id=");
      AppendQueryValue(&url, afterId);
    }

    HttpHeader headers[ANTHROPIC_HEADER_CAPACITY];
    u32 headerCount =
        Anthropic_BuildHeaders(arena, auth, nullptr, nullptr, headers);
    HttpRequest request = {};
    request.Method = SVL("GET");
    request.Url = StrBuf_View(&url);
    request.Headers = headers;
    request.HeaderCount = headerCount;

    HttpResponse response = fetch(fetchUser, arena, &request, abort);
    if (response.Status < 0) {
      StrView error = response.Error.Length ? response.Error : SVL("Anthropic model request failed");
      return AnthropicModelListError(arena, ProviderModelListStatus::Unavailable, error);
    }
    if (response.Status < 200 || response.Status >= 300) {
      return AnthropicModelListError(arena, ProviderModelListStatus::Failed,
                                     HttpStatusError(arena, response.Status));
    }

    AnthropicModelPage page = {};
    if (!Anthropic_ParseModelsResponse(arena, response.Body, &page)) {
      return AnthropicModelListError(arena, ProviderModelListStatus::Failed, page.Error);
    }
    for (u32 i = 0; i < page.Count; i++) Array_Push(&models, page.Models[i]);
    if (!page.HasMore) {
      ProviderModelList live = {};
      live.Status = ProviderModelListStatus::Success;
      live.Models = models.Data;
      live.Count = models.Count;
      return ProviderModelList_Merge(arena, live, nullptr, 0);
    }

    for (u32 i = 0; i < cursors.Count; i++) {
      if (StrView_Eq(cursors.Data[i], page.LastId)) {
        return AnthropicModelListError(arena, ProviderModelListStatus::Failed,
                                       SVL("Anthropic model pagination repeated a cursor"));
      }
    }
    afterId = StrView_Dup(arena, page.LastId);
    Array_Push(&cursors, afterId);
  }

  return AnthropicModelListError(arena, ProviderModelListStatus::Failed,
                                 SVL("Anthropic model pagination exceeded 100 pages"));
}

ProviderModelList Anthropic_ListModels(Arena* arena, ProviderAuth auth,
                                       volatile i32* abort) {
  return Anthropic_ListModelsWithFetch(arena, auth, abort,
                                       AnthropicHttpFetch, nullptr);
}

Provider Anthropic_Provider() {
  Provider p = {};
  p.Ctx = nullptr;
  p.Stream = Anthropic_Stream;
  return p;
}
