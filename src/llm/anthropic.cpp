#include "base/str.h"
#include "llm/anthropic.h"
#include "net/http.h"

// Anthropic Messages provider: HTTP glue around the protocol module.

static bool OnHttpData(void* user, StrView chunk) {
  AnthropicParse_Feed((AnthropicParse*)user, chunk);
  return true;
}

static bool Anthropic_Stream(void* ctx, Arena* arena, const Model* model, const LlmContext* context,
                             const StreamOptions* options, StreamSink sink, Message* out) {
  (void)ctx;
  StrBuf body = Anthropic_BuildRequest(arena, model, context, options);

  StrBuf url = StrBuf_Make(arena, model->BaseUrl.Length + 16);
  StrBuf_Append(&url, model->BaseUrl);
  StrBuf_AppendCStr(&url, "/v1/messages");

  StrView token = options ? options->ApiKey : StrView{"", 0};
  bool isOAuth = StrView_Contains(token, "sk-ant-oat");

  HttpHeader headers[6];
  u32 hc = 0;
  headers[hc++] = HttpHeader{SVL("anthropic-version"), SVL("2023-06-01")};
  headers[hc++] = HttpHeader{SVL("content-type"), SVL("application/json")};
  if (isOAuth) {
    StrBuf bearer = StrBuf_Make(arena, token.Length + 8);
    StrBuf_AppendCStr(&bearer, "Bearer ");
    StrBuf_Append(&bearer, token);
    headers[hc++] = HttpHeader{SVL("authorization"), StrBuf_View(&bearer)};
    headers[hc++] = HttpHeader{SVL("anthropic-beta"), SVL("claude-code-20250219,oauth-2025-04-20")};
    headers[hc++] = HttpHeader{SVL("user-agent"), SVL("claude-cli/2.1.75")};
    headers[hc++] = HttpHeader{SVL("x-app"), SVL("cli")};
  } else {
    headers[hc++] = HttpHeader{SVL("x-api-key"), token};
  }

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

  HttpStreamSink hsink = {};
  hsink.User = &st;
  hsink.OnData = OnHttpData;

  HttpResult r = Http_Stream(arena, &req, hsink, options ? options->Abort : nullptr);

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
      if (st.Buffer.Length) {
        StrBuf_AppendCStr(&e, ": ");
        StrBuf_Append(&e, StrBuf_View(&st.Buffer));
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

Provider Anthropic_Provider() {
  Provider p = {};
  p.Ctx = nullptr;
  p.Stream = Anthropic_Stream;
  return p;
}
