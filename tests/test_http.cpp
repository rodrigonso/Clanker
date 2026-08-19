// The default http suite contains pure retry-policy tests and local-loopback
// integration checks. TestHttpLive is the only test that requires connectivity.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/arena.h"
#include "base/thread.h"
#include "check.h"
#include "net/http.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int clank_socklen_t;
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
typedef socklen_t clank_socklen_t;
#define INVALID_SOCKET (-1)
#endif

namespace {

// ---- pure header/status-line parsing ---------------------------------------

void TestStatusLineParsing() {
  CHECK(HttpRetryMeta_ParseStatusLine(SVL("HTTP/1.1 200 OK")) == 200);
  CHECK(HttpRetryMeta_ParseStatusLine(SVL("HTTP/1.1 429 Too Many Requests\r\n")) == 429);
  CHECK(HttpRetryMeta_ParseStatusLine(SVL("HTTP/2 500\r\n")) == 500);
  CHECK(HttpRetryMeta_ParseStatusLine(SVL("HTTP/1.0 404 Not Found")) == 404);
  CHECK(HttpRetryMeta_ParseStatusLine(SVL("X-Should-Retry: true")) == -1);
  CHECK(HttpRetryMeta_ParseStatusLine(SVL("")) == -1);
  CHECK(HttpRetryMeta_ParseStatusLine(SVL("HTTP/1.1")) == -1);
}

void TestHeaderLineParsing() {
  // x-should-retry, case-insensitive name and value.
  {
    HttpRetryMeta meta = {};
    meta.RetryAfterMs = -1;
    HttpRetryMeta_ParseHeaderLine(&meta, SVL("X-Should-Retry: true"));
    CHECK_ALL(meta.HasShouldRetry, meta.ShouldRetry);
  }
  {
    HttpRetryMeta meta = {};
    meta.RetryAfterMs = -1;
    HttpRetryMeta_ParseHeaderLine(&meta, SVL("x-should-retry: FALSE"));
    CHECK_ALL(meta.HasShouldRetry, !meta.ShouldRetry);
  }
  {
    // Unrecognized value: no opinion recorded.
    HttpRetryMeta meta = {};
    meta.RetryAfterMs = -1;
    HttpRetryMeta_ParseHeaderLine(&meta, SVL("X-Should-Retry: maybe"));
    CHECK(!meta.HasShouldRetry);
  }
  // retry-after-ms in milliseconds.
  {
    HttpRetryMeta meta = {};
    meta.RetryAfterMs = -1;
    HttpRetryMeta_ParseHeaderLine(&meta, SVL("Retry-After-Ms: 1234"));
    CHECK(meta.RetryAfterMs == 1234);
  }
  // retry-after in seconds -> converted to ms.
  {
    HttpRetryMeta meta = {};
    meta.RetryAfterMs = -1;
    HttpRetryMeta_ParseHeaderLine(&meta, SVL("Retry-After: 3"));
    CHECK(meta.RetryAfterMs == 3000);
  }
  // retry-after-ms takes priority over a subsequent retry-after.
  {
    HttpRetryMeta meta = {};
    meta.RetryAfterMs = -1;
    HttpRetryMeta_ParseHeaderLine(&meta, SVL("Retry-After-Ms: 500"));
    HttpRetryMeta_ParseHeaderLine(&meta, SVL("Retry-After: 30"));
    CHECK(meta.RetryAfterMs == 500);
  }
  // ...and priority holds regardless of header order.
  {
    HttpRetryMeta meta = {};
    meta.RetryAfterMs = -1;
    HttpRetryMeta_ParseHeaderLine(&meta, SVL("Retry-After: 30"));
    HttpRetryMeta_ParseHeaderLine(&meta, SVL("Retry-After-Ms: 500"));
    CHECK(meta.RetryAfterMs == 500);
  }
  // Unrelated headers and status lines are ignored, not misparsed.
  {
    HttpRetryMeta meta = {};
    meta.RetryAfterMs = -1;
    HttpRetryMeta_ParseHeaderLine(&meta, SVL("Content-Type: application/json"));
    HttpRetryMeta_ParseHeaderLine(&meta, SVL("HTTP/1.1 429 Too Many Requests"));
    CHECK_ALL(!meta.HasShouldRetry, meta.RetryAfterMs == -1);
  }
  // Malformed values are ignored (no colon, non-numeric).
  {
    HttpRetryMeta meta = {};
    meta.RetryAfterMs = -1;
    HttpRetryMeta_ParseHeaderLine(&meta, SVL("Retry-After-Ms: soon"));
    HttpRetryMeta_ParseHeaderLine(&meta, SVL("NoColonHere"));
    CHECK(meta.RetryAfterMs == -1);
  }
}

// ---- pure retry decision + delay --------------------------------------------

void TestHttpStatusIsRetryable() {
  CHECK_ALL(HttpStatus_IsRetryable(408), HttpStatus_IsRetryable(409), HttpStatus_IsRetryable(429),
            HttpStatus_IsRetryable(500), HttpStatus_IsRetryable(503), HttpStatus_IsRetryable(599));
  CHECK_ALL(!HttpStatus_IsRetryable(200), !HttpStatus_IsRetryable(400), !HttpStatus_IsRetryable(401),
            !HttpStatus_IsRetryable(404), !HttpStatus_IsRetryable(600), !HttpStatus_IsRetryable(-1));
}

void TestShouldRetryDecisions() {
  HttpRetryPolicy off = HttpRetryPolicy_Default();
  HttpRetryMeta noMeta = {};
  noMeta.RetryAfterMs = -1;

  // Default-off: never retries regardless of status.
  CHECK(!HttpRetry_ShouldRetry(&off, 500, false, &noMeta, 1));
  CHECK(!HttpRetry_ShouldRetry(nullptr, 500, false, &noMeta, 1));

  HttpRetryPolicy on = off;
  on.Enabled = true;

  // Transport failure is always eligible while attempts remain.
  CHECK(HttpRetry_ShouldRetry(&on, -1, true, &noMeta, 1));
  // Retryable status codes are eligible.
  CHECK_ALL(HttpRetry_ShouldRetry(&on, 429, false, &noMeta, 1),
            HttpRetry_ShouldRetry(&on, 500, false, &noMeta, 1),
            HttpRetry_ShouldRetry(&on, 408, false, &noMeta, 1),
            HttpRetry_ShouldRetry(&on, 409, false, &noMeta, 1));
  // Non-retryable statuses are not.
  CHECK_ALL(!HttpRetry_ShouldRetry(&on, 400, false, &noMeta, 1),
            !HttpRetry_ShouldRetry(&on, 404, false, &noMeta, 1),
            !HttpRetry_ShouldRetry(&on, 200, false, &noMeta, 1));

  // Exhausted attempts (MaxRetries=2 -> 3 total tries) stop retrying.
  CHECK(HttpRetry_ShouldRetry(&on, 500, false, &noMeta, 1));
  CHECK(HttpRetry_ShouldRetry(&on, 500, false, &noMeta, 2));
  CHECK(!HttpRetry_ShouldRetry(&on, 500, false, &noMeta, 3));

  // Explicit x-should-retry overrides the default classification both ways.
  HttpRetryMeta forceTrue = {};
  forceTrue.HasShouldRetry = true;
  forceTrue.ShouldRetry = true;
  forceTrue.RetryAfterMs = -1;
  CHECK(HttpRetry_ShouldRetry(&on, 400, false, &forceTrue, 1));  // otherwise non-retryable status

  HttpRetryMeta forceFalse = {};
  forceFalse.HasShouldRetry = true;
  forceFalse.ShouldRetry = false;
  forceFalse.RetryAfterMs = -1;
  CHECK(!HttpRetry_ShouldRetry(&on, 500, false, &forceFalse, 1));  // otherwise retryable status
  CHECK(!HttpRetry_ShouldRetry(&on, -1, true, &forceFalse, 1));    // even a transport failure
}

void TestDelayMsComputation() {
  HttpRetryPolicy p = HttpRetryPolicy_Default();
  p.Enabled = true;  // Enabled doesn't affect delay math, but keep it realistic

  HttpRetryMeta noMeta = {};
  noMeta.RetryAfterMs = -1;

  // Exponential fallback: base 500ms doubling, capped at 8000ms, +/-25% jitter.
  CHECK(HttpRetry_DelayMs(&p, &noMeta, 0, 0.0) == 375);   // 500 * 0.75
  CHECK(HttpRetry_DelayMs(&p, &noMeta, 0, 1.0) == 625);   // 500 * 1.25
  CHECK(HttpRetry_DelayMs(&p, &noMeta, 1, 0.0) == 750);   // 1000 * 0.75
  CHECK(HttpRetry_DelayMs(&p, &noMeta, 2, 0.0) == 1500);  // 2000 * 0.75
  // Enough doublings to hit the cap before jitter is applied; the final
  // delay remains capped even when positive jitter would exceed it.
  CHECK(HttpRetry_DelayMs(&p, &noMeta, 10, 0.0) == 6000);   // 8000 * 0.75
  CHECK(HttpRetry_DelayMs(&p, &noMeta, 10, 1.0) == 8000);

  // Server-provided delay wins outright (no jitter applied), clamped to MaxDelayMs.
  HttpRetryMeta server = {};
  server.RetryAfterMs = 2500;
  CHECK(HttpRetry_DelayMs(&p, &server, 0, 0.5) == 2500);

  HttpRetryMeta tooLong = {};
  tooLong.RetryAfterMs = 60000;
  CHECK(HttpRetry_DelayMs(&p, &tooLong, 0, 0.5) == 8000);

  // A configurable max delay clamps both server and computed paths.
  HttpRetryPolicy tight = p;
  tight.MaxDelayMs = 1000;
  CHECK(HttpRetry_DelayMs(&tight, &server, 0, 0.5) == 1000);
  CHECK(HttpRetry_DelayMs(&tight, &noMeta, 5, 0.0) == 750);  // 1000 * 0.75
}

// ---- retry loop via dependency injection (no sockets, no real sleep) -------

struct FakeAttempt {
  i32 Status;
  bool BodyStarted;
  bool HasShouldRetry;
  bool ShouldRetry;
};

struct FakeTransportState {
  const FakeAttempt* Attempts;
  u32 Count;
  u32 Calls;
  u32 SleepCalls;
  u32 TotalSleptMs;
  volatile i32* AbortAfterSleep;  // optional: set *abort after N sleep calls
  u32 AbortAfterSleepCalls;
  u32 RetryStarts;
  u32 RetryEnds;
  u32 RetrySequence;
  u32 LastRetryAttempt;
  i32 LastRetryStatus;
};

HttpResult FakeTransport(void* user, Arena*, const HttpRequest*, HttpStreamSink, volatile i32*) {
  FakeTransportState* s = (FakeTransportState*)user;
  u32 i = s->Calls < s->Count ? s->Calls : s->Count - 1;
  s->Calls++;
  const FakeAttempt& a = s->Attempts[i];
  HttpResult r = {};
  r.Status = a.Status;
  r.BodyStarted = a.BodyStarted;
  r.Meta.HasShouldRetry = a.HasShouldRetry;
  r.Meta.ShouldRetry = a.ShouldRetry;
  r.Meta.RetryAfterMs = -1;
  return r;
}

void FakeSleep(void* user, u32 ms) {
  FakeTransportState* s = (FakeTransportState*)user;
  s->SleepCalls++;
  s->TotalSleptMs += ms;
  if (s->AbortAfterSleep && s->SleepCalls >= s->AbortAfterSleepCalls) *s->AbortAfterSleep = 1;
}

double FakeRandZero(void*) { return 0.0; }

void FakeRetryEvent(void* user, const HttpRetryEvent* event) {
  FakeTransportState* s = (FakeTransportState*)user;
  if (event->Kind == HttpRetryEventKind::Start) {
    s->RetryStarts++;
    s->RetrySequence = s->RetrySequence * 10 + 1;
  } else {
    s->RetryEnds++;
    s->RetrySequence = s->RetrySequence * 10 + 2;
  }
  s->LastRetryAttempt = event->Attempt;
  s->LastRetryStatus = event->Status;
}

void TestRetryLoopStopsAfterRetryableFailures() {
  Arena a = ArenaCreate();
  HttpRetryPolicy policy = HttpRetryPolicy_Default();
  policy.Enabled = true;
  policy.MaxRetries = 2;
  policy.BaseDelayMs = 10;
  policy.MaxDelayMs = 1000;

  FakeAttempt attempts[3] = {
      {500, false, false, false},
      {429, false, false, false},
      {200, true, false, false},
  };
  FakeTransportState state = {};
  state.Attempts = attempts;
  state.Count = 3;
  policy.EventUser = &state;
  policy.OnEvent = FakeRetryEvent;

  HttpRetryDeps deps = {};
  deps.Transport = FakeTransport;
  deps.TransportUser = &state;
  deps.Sleep = FakeSleep;
  deps.SleepUser = &state;
  deps.Rand = FakeRandZero;

  HttpRequest req = {};
  req.Method = SVL("GET");
  req.Url = SVL("http://example.invalid/");
  HttpStreamSink sink = {};

  HttpResult r = Http_StreamWithRetryDeps(&a, &req, sink, nullptr, &policy, &deps);
  CHECK_ALL(state.Calls == 3, r.Status == 200, r.BodyStarted, state.SleepCalls == 2,
            state.RetryStarts == 2, state.RetryEnds == 2,
            state.RetrySequence == 1212, state.LastRetryAttempt == 2,
            state.LastRetryStatus == 429);

  ArenaRelease(&a);
}

void TestRetryLoopNeverRetriesAfterBodyStarted() {
  Arena a = ArenaCreate();
  HttpRetryPolicy policy = HttpRetryPolicy_Default();
  policy.Enabled = true;
  policy.BaseDelayMs = 10;

  // A transport failure that nonetheless began forwarding a 2xx body (e.g. the
  // connection dropped mid-stream) must never be retried, even though a bare
  // transport failure would otherwise always qualify.
  FakeAttempt attempts[1] = {{-1, true, false, false}};
  FakeTransportState state = {};
  state.Attempts = attempts;
  state.Count = 1;

  HttpRetryDeps deps = {};
  deps.Transport = FakeTransport;
  deps.TransportUser = &state;
  deps.Sleep = FakeSleep;
  deps.SleepUser = &state;
  deps.Rand = FakeRandZero;

  HttpRequest req = {};
  HttpStreamSink sink = {};
  HttpResult r = Http_StreamWithRetryDeps(&a, &req, sink, nullptr, &policy, &deps);
  CHECK_ALL(state.Calls == 1, state.SleepCalls == 0, r.Status == -1, r.BodyStarted);

  ArenaRelease(&a);
}

void TestRetryLoopHonorsShouldRetryFalse() {
  Arena a = ArenaCreate();
  HttpRetryPolicy policy = HttpRetryPolicy_Default();
  policy.Enabled = true;
  policy.BaseDelayMs = 10;

  // 429 would normally be retryable, but an explicit x-should-retry:false
  // must suppress it.
  FakeAttempt attempts[1] = {{429, false, true, false}};
  FakeTransportState state = {};
  state.Attempts = attempts;
  state.Count = 1;

  HttpRetryDeps deps = {};
  deps.Transport = FakeTransport;
  deps.TransportUser = &state;
  deps.Sleep = FakeSleep;
  deps.SleepUser = &state;
  deps.Rand = FakeRandZero;

  HttpRequest req = {};
  HttpStreamSink sink = {};
  HttpResult r = Http_StreamWithRetryDeps(&a, &req, sink, nullptr, &policy, &deps);
  CHECK_ALL(state.Calls == 1, state.SleepCalls == 0, r.Status == 429);

  ArenaRelease(&a);
}

void TestRetryLoopDisabledPolicyIsPassthrough() {
  Arena a = ArenaCreate();
  HttpRetryPolicy policy = HttpRetryPolicy_Default();  // Enabled == false

  FakeAttempt attempts[1] = {{500, false, false, false}};
  FakeTransportState state = {};
  state.Attempts = attempts;
  state.Count = 1;

  HttpRetryDeps deps = {};
  deps.Transport = FakeTransport;
  deps.TransportUser = &state;
  deps.Sleep = FakeSleep;
  deps.SleepUser = &state;
  deps.Rand = FakeRandZero;

  HttpRequest req = {};
  HttpStreamSink sink = {};
  HttpResult r = Http_StreamWithRetryDeps(&a, &req, sink, nullptr, &policy, &deps);
  CHECK_ALL(state.Calls == 1, state.SleepCalls == 0, r.Status == 500);

  // nullptr policy behaves the same as an explicit disabled one.
  FakeTransportState state2 = {};
  state2.Attempts = attempts;
  state2.Count = 1;
  deps.TransportUser = &state2;
  deps.SleepUser = &state2;
  HttpResult r2 = Http_StreamWithRetryDeps(&a, &req, sink, nullptr, nullptr, &deps);
  CHECK_ALL(state2.Calls == 1, r2.Status == 500);

  ArenaRelease(&a);
}

void TestRetryLoopAbortDuringBackoffStopsRetrying() {
  Arena a = ArenaCreate();
  HttpRetryPolicy policy = HttpRetryPolicy_Default();
  policy.Enabled = true;
  policy.MaxRetries = 5;
  policy.BaseDelayMs = 1000;  // several 100ms sleep increments per attempt

  FakeAttempt attempts[1] = {{500, false, false, false}};  // always retryable
  FakeTransportState state = {};
  state.Attempts = attempts;
  state.Count = 1;

  volatile i32 abort = 0;
  state.AbortAfterSleep = &abort;
  state.AbortAfterSleepCalls = 2;  // cancel partway through the first backoff
  policy.EventUser = &state;
  policy.OnEvent = FakeRetryEvent;

  HttpRetryDeps deps = {};
  deps.Transport = FakeTransport;
  deps.TransportUser = &state;
  deps.Sleep = FakeSleep;
  deps.SleepUser = &state;
  deps.Rand = FakeRandZero;

  HttpRequest req = {};
  HttpStreamSink sink = {};
  HttpResult r = Http_StreamWithRetryDeps(&a, &req, sink, &abort, &policy, &deps);

  // Only one transport attempt: the abort fired while sleeping before a
  // second attempt could be made, and it isn't masked by another retry.
  CHECK_ALL(state.Calls == 1, r.Status == -1,
            StrView_EqCStr(r.Error, "aborted"), state.SleepCalls == 2,
            state.RetryStarts == 1, state.RetryEnds == 1,
            state.RetrySequence == 12);

  ArenaRelease(&a);
}

// ---- real local-socket integration: header capture + body suppression -----

#if defined(_WIN32)
bool NetInit() {
  static bool up = false;
  if (up) return true;
  WSADATA d;
  if (WSAStartup(MAKEWORD(2, 2), &d) != 0) return false;
  up = true;
  return true;
}
void CloseSock(SOCKET fd) { closesocket(fd); }
#else
bool NetInit() { return true; }
void CloseSock(SOCKET fd) { close(fd); }
#endif

void SendAll(SOCKET c, const char* data, u32 len) {
  u32 off = 0;
  while (off < len) {
    int n = (int)send(c, data + off, (int)(len - off), 0);
    if (n <= 0) break;
    off += (u32)n;
  }
}

// Bind an ephemeral 127.0.0.1 listener; returns the socket (or INVALID_SOCKET).
SOCKET OpenLoopbackListener(u16* outPort) {
  if (!NetInit()) return INVALID_SOCKET;
  SOCKET fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd == INVALID_SOCKET) return fd;
  int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
  sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(fd, 4) != 0) {
    CloseSock(fd);
    return INVALID_SOCKET;
  }
  sockaddr_in bound;
  memset(&bound, 0, sizeof(bound));
  clank_socklen_t bl = sizeof(bound);
  getsockname(fd, (sockaddr*)&bound, &bl);
  *outPort = ntohs(bound.sin_port);
  return fd;
}

struct OneShotServerCtx {
  SOCKET ListenFd;
  StrView RawResponse;  // full "HTTP/1.1 ...\r\n...\r\n\r\n<body>" bytes to send back
};

void ServeOneRequest(void* arg) {
  OneShotServerCtx* ctx = (OneShotServerCtx*)arg;
  SOCKET c = accept(ctx->ListenFd, nullptr, nullptr);
  if (c == INVALID_SOCKET) return;
  char buf[4096];
  int total = 0;
  for (;;) {
    int n = (int)recv(c, buf + total, (int)(sizeof(buf) - 1 - total), 0);
    if (n <= 0) break;
    total += n;
    buf[total] = '\0';
    if (total >= 4 && strstr(buf, "\r\n\r\n")) break;
    if (total >= (int)sizeof(buf) - 1) break;
  }
  SendAll(c, ctx->RawResponse.Data, ctx->RawResponse.Length);
  CloseSock(c);
}

struct Counter {
  u32 Calls;
  u64 Bytes;
};

bool CountingOnData(void* user, StrView chunk) {
  Counter* c = (Counter*)user;
  c->Calls++;
  c->Bytes += chunk.Length;
  return true;
}

StrView BuildResponse(Arena* a, const char* statusLine, const char* extraHeaders, StrView body) {
  StrBuf b = StrBuf_Make(a, body.Length + 256);
  StrBuf_AppendCStr(&b, statusLine);
  StrBuf_AppendCStr(&b, "\r\n");
  StrBuf_AppendCStr(&b, extraHeaders);
  StrBuf_AppendCStr(&b, "Content-Length: ");
  StrBuf_AppendU64(&b, body.Length);
  StrBuf_AppendCStr(&b, "\r\nConnection: close\r\n\r\n");
  StrBuf_Append(&b, body);
  return StrBuf_View(&b);
}

// A retryable, non-2xx response with should-retry/retry-after-ms headers:
// when the caller opts into BufferNonSuccessBody, the sink must never see any
// of these bytes, and Http_Stream must still surface Status/Meta/Body.
void TestRealServerBuffersNonSuccessBody() {
  Arena a = ArenaCreate();
  Http_GlobalInit();

  u16 port = 0;
  SOCKET listenFd = OpenLoopbackListener(&port);
  CHECK(listenFd != INVALID_SOCKET);
  if (listenFd == INVALID_SOCKET) {
    ArenaRelease(&a);
    return;
  }

  StrView body = SVL("{\"error\":\"rate limited\"}");
  StrView response = BuildResponse(
      &a, "HTTP/1.1 429 Too Many Requests",
      "Content-Type: application/json\r\nX-Should-Retry: true\r\nRetry-After-Ms: 1234\r\n", body);

  OneShotServerCtx ctx = {listenFd, response};
  Thread server = {};
  CHECK(Thread_Start(&server, ServeOneRequest, &ctx));

  StrBuf url = StrBuf_Make(&a, 64);
  StrBuf_AppendCStr(&url, "http://127.0.0.1:");
  StrBuf_AppendU64(&url, port);
  StrBuf_AppendCStr(&url, "/v1/chat/completions");

  Counter counter = {};
  HttpStreamSink sink = {};
  sink.User = &counter;
  sink.OnData = CountingOnData;

  HttpRequest req = {};
  req.Method = SVL("GET");
  req.Url = StrBuf_View(&url);
  req.BufferNonSuccessBody = true;

  HttpResult r = Http_Stream(&a, &req, sink, nullptr);
  Thread_Join(&server);
  CloseSock(listenFd);

  CHECK_ALL(r.Status == 429, !r.BodyStarted, counter.Calls == 0, counter.Bytes == 0,
            r.Meta.HasShouldRetry, r.Meta.ShouldRetry, r.Meta.RetryAfterMs == 1234,
            StrView_Eq(r.Body, body));

  Http_GlobalShutdown();
  ArenaRelease(&a);
}

// The same non-2xx response, but with BufferNonSuccessBody left false (the
// default): existing callers like Http_Fetch rely on every byte reaching the
// sink regardless of status, and that behavior must be unchanged.
void TestRealServerDefaultForwardsNonSuccessBody() {
  Arena a = ArenaCreate();
  Http_GlobalInit();

  u16 port = 0;
  SOCKET listenFd = OpenLoopbackListener(&port);
  CHECK(listenFd != INVALID_SOCKET);
  if (listenFd == INVALID_SOCKET) {
    ArenaRelease(&a);
    return;
  }

  StrView body = SVL("internal error");
  StrView response = BuildResponse(&a, "HTTP/1.1 500 Internal Server Error", "", body);

  OneShotServerCtx ctx = {listenFd, response};
  Thread server = {};
  CHECK(Thread_Start(&server, ServeOneRequest, &ctx));

  StrBuf url = StrBuf_Make(&a, 64);
  StrBuf_AppendCStr(&url, "http://127.0.0.1:");
  StrBuf_AppendU64(&url, port);
  StrBuf_AppendCStr(&url, "/");

  Counter counter = {};
  HttpStreamSink sink = {};
  sink.User = &counter;
  sink.OnData = CountingOnData;

  HttpRequest req = {};
  req.Method = SVL("GET");
  req.Url = StrBuf_View(&url);
  // req.BufferNonSuccessBody left false (default).

  HttpResult streamed = Http_Stream(&a, &req, sink, nullptr);
  Thread_Join(&server);
  CloseSock(listenFd);

  CHECK_ALL(streamed.Status == 500, streamed.BodyStarted,
            counter.Calls > 0, counter.Bytes == body.Length);

  Http_GlobalShutdown();
  ArenaRelease(&a);
}

// A successful (2xx) response is always forwarded to the sink, and
// BodyStarted is set, regardless of BufferNonSuccessBody.
void TestRealServerSuccessStreamsToSink() {
  Arena a = ArenaCreate();
  Http_GlobalInit();

  u16 port = 0;
  SOCKET listenFd = OpenLoopbackListener(&port);
  CHECK(listenFd != INVALID_SOCKET);
  if (listenFd == INVALID_SOCKET) {
    ArenaRelease(&a);
    return;
  }

  StrView body = SVL("hello streamed body");
  StrView response = BuildResponse(&a, "HTTP/1.1 200 OK", "", body);

  OneShotServerCtx ctx = {listenFd, response};
  Thread server = {};
  CHECK(Thread_Start(&server, ServeOneRequest, &ctx));

  StrBuf url = StrBuf_Make(&a, 64);
  StrBuf_AppendCStr(&url, "http://127.0.0.1:");
  StrBuf_AppendU64(&url, port);
  StrBuf_AppendCStr(&url, "/");

  Counter counter = {};
  HttpStreamSink sink = {};
  sink.User = &counter;
  sink.OnData = CountingOnData;

  HttpRequest req = {};
  req.Method = SVL("GET");
  req.Url = StrBuf_View(&url);
  req.BufferNonSuccessBody = true;  // irrelevant for a 2xx response

  HttpResult r = Http_Stream(&a, &req, sink, nullptr);
  Thread_Join(&server);
  CloseSock(listenFd);

  CHECK_ALL(r.Status == 200, r.BodyStarted, counter.Calls > 0, counter.Bytes == body.Length,
            r.Body.Length == 0);

  Http_GlobalShutdown();
  ArenaRelease(&a);
}

// ---- live network smoke test (unchanged behavior) --------------------------

bool LiveOnData(void* user, StrView chunk) {
  ((Counter*)user)->Bytes += chunk.Length;
  return true;
}

void TestLiveHttp() {
  // Overridable so the suite can be pointed at a local server in sandboxed or
  // offline environments.
  const char* url = getenv("CLANK_TEST_HTTP_URL");
  if (!url) url = "https://example.com";

  Http_GlobalInit();
  Arena a = ArenaCreate();

  HttpRequest req = {};
  req.Method = SV("GET");
  req.Url = SV(url);

  Counter c = {};
  HttpStreamSink sink = {};
  sink.User = &c;
  sink.OnData = LiveOnData;

  HttpResult streamed = Http_Stream(&a, &req, sink, nullptr);
  if (streamed.Status < 200 || streamed.Status >= 400) {
    printf("  http: GET %s failed status=%d err=%.*s\n", url, streamed.Status,
           (int)streamed.Error.Length,
           streamed.Error.Data ? streamed.Error.Data : "");
  }
  CHECK_ALL(streamed.Status >= 200, streamed.Status < 400, c.Bytes > 0);

  // The non-streaming path shares the same curl backend; check it agrees.
  HttpResponse fetched = Http_Fetch(&a, &req, nullptr);
  CHECK_ALL(fetched.Status >= 200, fetched.Status < 400,
            fetched.Body.Length > 0);

  ArenaRelease(&a);
  Http_GlobalShutdown();
}

}  // namespace

void TestHttp() {
  TestStatusLineParsing();
  TestHeaderLineParsing();
  TestHttpStatusIsRetryable();
  TestShouldRetryDecisions();
  TestDelayMsComputation();
  TestRetryLoopStopsAfterRetryableFailures();
  TestRetryLoopNeverRetriesAfterBodyStarted();
  TestRetryLoopHonorsShouldRetryFalse();
  TestRetryLoopDisabledPolicyIsPassthrough();
  TestRetryLoopAbortDuringBackoffStopsRetrying();
  TestRealServerBuffersNonSuccessBody();
  TestRealServerDefaultForwardsNonSuccessBody();
  TestRealServerSuccessStreamsToSink();
}

void TestHttpLive() {
  TestLiveHttp();
}
