#pragma once
#include "base/arena.h"
#include "base/str.h"

// Thin streaming HTTP client abstraction. The implementation (http_curl.cpp)
// uses libcurl. Response bytes are delivered to the sink as they arrive
// (zero-copy slices into curl's buffer), which the caller frames as SSE.

struct HttpHeader {
  StrView Name;
  StrView Value;
};

// "authorization: Bearer <token>", the shape every provider here uses.
inline HttpHeader Http_BearerHeader(Arena* arena, StrView token) {
  StrBuf bearer = StrBuf_Make(arena, token.Length + 8);
  StrBuf_AppendCStr(&bearer, "Bearer ");
  StrBuf_Append(&bearer, token);
  return HttpHeader{SVL("authorization"), StrBuf_View(&bearer)};
}

struct HttpRequest {
  StrView Method;  // "POST" or "GET"
  StrView Url;
  const HttpHeader* Headers;
  u32 HeaderCount;
  StrView Body;  // request body (for POST)
  bool FollowRedirects;
  // When true, a non-2xx response body is withheld from the streaming sink
  // and buffered internally instead (see HttpResult::Body); the sink only
  // ever sees bytes from a successful (2xx) response. Existing callers that
  // want every byte via `sink` regardless of status (e.g. Http_Fetch based
  // tool downloads) leave this false, the default. Streaming SSE parsers set
  // it so a retryable error page's bytes never reach the event parser (see
  // ChatParse_Feed).
  bool BufferNonSuccessBody;
};

struct HttpStreamSink {
  void* User;
  // Return false to abort the transfer.
  bool (*OnData)(void* user, StrView chunk);
};

// Response-header-derived retry hints, captured before any body byte is
// forwarded to the sink (headers always arrive before the body). Populated
// whenever Status >= 0 (i.e. a response was actually received).
struct HttpRetryMeta {
  bool HasShouldRetry;  // true if an "x-should-retry" header was present
  bool ShouldRetry;     // its value, when HasShouldRetry is true
  i64 RetryAfterMs;     // parsed "retry-after-ms" (ms) or "retry-after" (s -> ms); -1 if absent/unparseable
};

// Parse one CRLF/LF-terminated (or bare) HTTP status line, e.g.
// "HTTP/1.1 429 Too Many Requests". Returns -1 if `line` is not a status
// line. Pure/offline-testable.
i32 HttpRetryMeta_ParseStatusLine(StrView line);

// Parse one raw response header line (e.g. "X-Should-Retry: true"),
// case-insensitively recognizing x-should-retry ("true"/"false"),
// retry-after-ms (integer milliseconds), and retry-after (integer seconds;
// HTTP-date values are not supported and are ignored). retry-after-ms takes
// priority over retry-after when both are present. Unrecognized header
// lines and status lines are ignored, so every raw line from a response can
// be fed through unconditionally. Pure/offline-testable.
void HttpRetryMeta_ParseHeaderLine(HttpRetryMeta* meta, StrView line);

struct HttpResult {
  i32 Status;         // HTTP status code, or -1 on transport error / abort
  StrView Error;      // transport error message (arena-owned) when Status < 0
  HttpRetryMeta Meta;  // header-derived retry hints; valid whenever Status >= 0
  // Buffered non-2xx response body (arena-owned), populated only when the
  // request set BufferNonSuccessBody and a non-2xx status was received.
  // Empty for successful (2xx) responses -- those bytes went to the sink.
  StrView Body;
  // True once a 2xx response began forwarding bytes to the sink during this
  // attempt (even if the transfer later failed, e.g. a connection drop
  // mid-stream). Once true, this attempt's response has partially reached
  // downstream consumers and must never be transparently retried.
  bool BodyStarted;
};

// Non-streaming request: accumulates the full response body. For request/response
// endpoints like model discovery (not SSE).
struct HttpResponse {
  i32 Status;
  StrView Body;
  StrView Error;
};

// Idempotent process-wide setup/teardown. Initialization may be deferred until
// immediately before the first network-capable operation.
void Http_GlobalInit();
void Http_GlobalShutdown();

// Perform a (streaming) request. Chunks are delivered to `sink` as received.
// If `abort` becomes non-zero mid-transfer, the request is aborted.
HttpResult Http_Stream(Arena* arena, const HttpRequest* req, HttpStreamSink sink, volatile i32* abort);

// Perform a request and return the full response body.
HttpResponse Http_Fetch(Arena* arena, const HttpRequest* req, volatile i32* abort);

// =============================================================================
// Provider-request (transport-level) retry policy. Default-off: callers must
// opt in by passing a policy with Enabled=true to Http_StreamWithRetry(Deps).
// Retries only transport failures (Status < 0) and HTTP 408/409/429/5xx,
// honoring an explicit "x-should-retry" header (true forces a retry even for
// an otherwise non-retryable outcome; false always suppresses one). The delay
// before each retry prefers a server-provided Retry-After(-Ms) value (clamped
// to MaxDelayMs), otherwise an exponential backoff from BaseDelayMs doubling
// per attempt, capped at MaxDelayMs (default 8000ms / 8s), with +/-25%
// jitter. A retry is never attempted once a prior attempt's response body has
// begun forwarding to the sink (HttpResult::BodyStarted) -- see
// Http_StreamWithRetryDeps.
// =============================================================================

enum class HttpRetryEventKind : u8 {
  Start,
  End,
};

struct HttpRetryEvent {
  HttpRetryEventKind Kind;
  u32 Attempt;     // 1 for the first retry
  u32 MaxRetries;
  u32 DelayMs;
  i32 Status;
  StrView Error;   // transport error or buffered non-success response body
};

typedef void (*HttpRetryEventFn)(void* user, const HttpRetryEvent* event);

struct HttpRetryPolicy {
  bool Enabled;    // default-off; caller opts in
  u32 MaxRetries;  // retry attempts allowed after the first try (default 2)
  u32 BaseDelayMs;  // initial backoff before doubling (default 500ms)
  u32 MaxDelayMs;   // clamps both server-supplied and computed backoff delays (default 8000ms)
  void* EventUser;
  HttpRetryEventFn OnEvent;
};

// Enabled=false, MaxRetries=2, BaseDelayMs=500, MaxDelayMs=8000.
HttpRetryPolicy HttpRetryPolicy_Default();

// Pure classification: is `status` one of the transport-retryable HTTP codes
// (408, 409, 429, 5xx)? Transport failures (status < 0) are handled by the
// caller separately -- see HttpRetry_ShouldRetry.
bool HttpStatus_IsRetryable(i32 status);

// Pure decision: given the policy, this attempt's outcome (status, whether it
// was a transport failure, and any "x-should-retry" hint), and the number of
// attempts made so far (including this one), decide whether to retry again.
bool HttpRetry_ShouldRetry(const HttpRetryPolicy* policy, i32 status, bool transportFailure,
                           const HttpRetryMeta* meta, u32 attemptsMade);

// Pure delay computation (milliseconds) before the next attempt. attemptIndex
// is 0 for the delay before the first retry, 1 before the second, etc.
// `jitterRandom01` is caller-supplied randomness in [0,1) so the computation
// stays pure/testable; production callers pass a real draw. `meta` supplies a
// server-provided delay when present (clamped to policy->MaxDelayMs) which
// takes priority over the computed exponential backoff.
u32 HttpRetry_DelayMs(const HttpRetryPolicy* policy, const HttpRetryMeta* meta, u32 attemptIndex,
                     double jitterRandom01);

// Injection points so the retry loop is testable without real sockets, sleeps,
// or randomness (mirrors the AnthropicModelsFetchFn injection pattern).
typedef HttpResult (*HttpTransportFn)(void* user, Arena* arena, const HttpRequest* req,
                                      HttpStreamSink sink, volatile i32* abort);
typedef void (*HttpRetrySleepFn)(void* user, u32 ms);
typedef double (*HttpRetryRandFn)(void* user);  // must return a value in [0,1)

struct HttpRetryDeps {
  HttpTransportFn Transport;
  void* TransportUser;
  HttpRetrySleepFn Sleep;
  void* SleepUser;
  HttpRetryRandFn Rand;
  void* RandUser;
};

// Retrying stream driver. `policy == nullptr` or `!policy->Enabled` performs
// exactly one attempt (pure passthrough to `deps->Transport`). Sleeps are
// performed in <=100ms increments checking `*abort` between them, so
// cancellation during a backoff wait remains responsive. When OnEvent is set,
// a matched Start/End pair surrounds each backoff, including an interrupted
// backoff. Never retries once an attempt's HttpResult::BodyStarted is true.
HttpResult Http_StreamWithRetryDeps(Arena* arena, const HttpRequest* req, HttpStreamSink sink,
                                   volatile i32* abort, const HttpRetryPolicy* policy,
                                   const HttpRetryDeps* deps);

// Convenience wrapper using the real Http_Stream transport, a real (abort-
// aware) sleep, and the C library RNG for jitter.
HttpResult Http_StreamWithRetry(Arena* arena, const HttpRequest* req, HttpStreamSink sink,
                                volatile i32* abort, const HttpRetryPolicy* policy);
