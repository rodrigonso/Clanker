#include <curl/curl.h>
#include <stdlib.h>

#include "base/str.h"
#include "base/thread.h"
#include "net/http.h"

namespace {

// Reuse one serialized easy handle to retain connection, DNS, and TLS caches.
CURL* g_curl;
Mutex g_lock;
bool g_initialized;

// Case-insensitive ASCII equality (header names are ASCII per RFC 7230).
bool EqCI(StrView a, const char* b) {
  u32 bl = 0;
  while (b[bl]) bl++;
  if (a.Length != bl) return false;
  for (u32 i = 0; i < bl; i++) {
    char c = a.Data[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    char d = b[i];
    if (d >= 'A' && d <= 'Z') d = (char)(d - 'A' + 'a');
    if (c != d) return false;
  }
  return true;
}

StrView Trim(StrView s) {
  while (s.Length && (s.Data[0] == ' ' || s.Data[0] == '\t')) {
    s.Data++;
    s.Length--;
  }
  while (s.Length && (s.Data[s.Length - 1] == ' ' || s.Data[s.Length - 1] == '\t')) s.Length--;
  return s;
}

bool ParseU64(StrView s, u64* out) {
  if (s.Length == 0) return false;
  u64 v = 0;
  for (u32 i = 0; i < s.Length; i++) {
    char c = s.Data[i];
    if (c < '0' || c > '9') return false;
    v = v * 10 + (u64)(c - '0');
  }
  *out = v;
  return true;
}

// Strip a trailing CR/LF (curl's header callback includes the line ending).
StrView StripEol(StrView s) {
  while (s.Length && (s.Data[s.Length - 1] == '\n' || s.Data[s.Length - 1] == '\r')) s.Length--;
  return s;
}

struct XferCtx {
  Arena* mem;
  HttpStreamSink sink;
  volatile i32* abort;
  bool aborted;
  bool bufferNonSuccessBody;
  i32 status;          // -1 until a status line is seen
  bool bodyStarted;    // true once any body byte reached `sink`
  HttpRetryMeta meta;
  StrBuf errBody;      // buffered non-2xx body when bufferNonSuccessBody is set
};

size_t HeaderCb(char* buffer, size_t size, size_t nitems, void* userdata) {
  XferCtx* w = (XferCtx*)userdata;
  size_t n = size * nitems;
  StrView line = StripEol(StrView{buffer, (u32)n});
  if (line.Length == 0) return n;  // blank line: end of a header block
  if (line.Length >= 5 && memcmp(line.Data, "HTTP/", 5) == 0) {
    // A new status line starts a new header block (redirects, 100-continue).
    // Reset so only the final response's status and retry hints are kept.
    w->meta = HttpRetryMeta{};
    w->meta.RetryAfterMs = -1;
    w->errBody = {};
    w->status = HttpRetryMeta_ParseStatusLine(line);
    return n;
  }
  HttpRetryMeta_ParseHeaderLine(&w->meta, line);
  return n;
}

size_t WriteCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  XferCtx* w = (XferCtx*)userdata;
  size_t n = size * nmemb;
  if (w->abort && *w->abort) {
    w->aborted = true;
    return 0;
  }
  StrView chunk{ptr, (u32)n};
  bool success = w->status >= 200 && w->status < 300;
  if (!success && w->bufferNonSuccessBody) {
    if (!w->errBody.Data) w->errBody = StrBuf_Make(w->mem, 256);
    StrBuf_Append(&w->errBody, chunk);
    return n;
  }
  w->bodyStarted = true;
  if (w->sink.OnData && !w->sink.OnData(w->sink.User, chunk)) {
    w->aborted = true;
    return 0;
  }
  return n;
}

int XferCb(void* p, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  XferCtx* w = (XferCtx*)p;
  if (w->abort && *w->abort) {
    w->aborted = true;
    return 1;  // non-zero aborts the transfer
  }
  return 0;
}

}  // namespace

i32 HttpRetryMeta_ParseStatusLine(StrView line) {
  if (line.Length < 5 || memcmp(line.Data, "HTTP/", 5) != 0) return -1;
  u32 i = 5;
  while (i < line.Length && line.Data[i] != ' ') i++;  // skip "1.1"/"2"/etc.
  if (i >= line.Length) return -1;
  i++;  // skip the space
  u32 start = i;
  while (i < line.Length && line.Data[i] >= '0' && line.Data[i] <= '9') i++;
  if (i == start) return -1;
  i32 code = 0;
  for (u32 k = start; k < i; k++) code = code * 10 + (line.Data[k] - '0');
  return code;
}

void HttpRetryMeta_ParseHeaderLine(HttpRetryMeta* meta, StrView line) {
  u32 colon = 0;
  while (colon < line.Length && line.Data[colon] != ':') colon++;
  if (colon >= line.Length) return;
  StrView name = Trim(StrView{line.Data, colon});
  StrView value = Trim(StrView{line.Data + colon + 1, line.Length - colon - 1});
  if (EqCI(name, "x-should-retry")) {
    if (EqCI(value, "true")) {
      meta->HasShouldRetry = true;
      meta->ShouldRetry = true;
    } else if (EqCI(value, "false")) {
      meta->HasShouldRetry = true;
      meta->ShouldRetry = false;
    }
  } else if (EqCI(name, "retry-after-ms")) {
    u64 ms;
    if (ParseU64(value, &ms)) meta->RetryAfterMs = (i64)ms;
  } else if (EqCI(name, "retry-after")) {
    if (meta->RetryAfterMs < 0) {  // retry-after-ms already claimed priority
      u64 secs;
      if (ParseU64(value, &secs)) meta->RetryAfterMs = (i64)(secs * 1000);
    }
  }
}

void Http_GlobalInit() {
  if (g_initialized) return;
  curl_global_init(CURL_GLOBAL_DEFAULT);
  Mutex_Init(&g_lock);
  g_curl = curl_easy_init();
  g_initialized = true;
}

void Http_GlobalShutdown() {
  if (!g_initialized) return;
  if (g_curl) {
    curl_easy_cleanup(g_curl);
    g_curl = nullptr;
  }
  Mutex_Destroy(&g_lock);
  curl_global_cleanup();
  g_initialized = false;
}

HttpResult Http_Stream(Arena* arena, const HttpRequest* req, HttpStreamSink sink, volatile i32* abort) {
  HttpResult res = {};
  res.Status = -1;
  res.Meta.RetryAfterMs = -1;

  if (!g_curl) {
    res.Error = SV("curl init failed");
    return res;
  }

  Mutex_Lock(&g_lock);
  CURL* curl = g_curl;
  curl_easy_reset(curl);

  StrBuf url = StrBuf_Make(arena, req->Url.Length + 1);
  StrBuf_Append(&url, req->Url);
  curl_easy_setopt(curl, CURLOPT_URL, StrBuf_CStr(&url));

  if (StrView_EqCStr(req->Method, "POST")) {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->Body.Data);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)req->Body.Length);
  }

  struct curl_slist* hdrs = nullptr;
  for (u32 i = 0; i < req->HeaderCount; i++) {
    StrBuf h = StrBuf_Make(arena, req->Headers[i].Name.Length + req->Headers[i].Value.Length + 4);
    StrBuf_Append(&h, req->Headers[i].Name);
    StrBuf_AppendCStr(&h, ": ");
    StrBuf_Append(&h, req->Headers[i].Value);
    hdrs = curl_slist_append(hdrs, StrBuf_CStr(&h));
  }
  if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

  XferCtx w = {};
  w.mem = arena;
  w.sink = sink;
  w.abort = abort;
  w.bufferNonSuccessBody = req->BufferNonSuccessBody;
  w.status = -1;
  w.meta.RetryAfterMs = -1;
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCb);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &w);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &w);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, XferCb);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &w);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "clanker/0.1");
  if (req->FollowRedirects) {
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  }
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);

  CURLcode rc = curl_easy_perform(curl);
  if (rc == CURLE_OK) {
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    res.Status = (i32)code;
  } else if (w.aborted) {
    res.Error = SV("aborted");
  } else {
    res.Error = StrView_Dup(arena, SV(curl_easy_strerror(rc)));
  }
  res.Meta = w.meta;
  res.BodyStarted = w.bodyStarted;
  if (w.errBody.Data && w.errBody.Length) res.Body = StrBuf_View(&w.errBody);

  if (hdrs) curl_slist_free_all(hdrs);
  Mutex_Unlock(&g_lock);
  return res;
}

namespace {
bool FetchOnData(void* user, StrView chunk) {
  StrBuf_Append((StrBuf*)user, chunk);
  return true;
}
}  // namespace

HttpResponse Http_Fetch(Arena* arena, const HttpRequest* req, volatile i32* abort) {
  StrBuf buf = StrBuf_Make(arena, 1024);
  HttpStreamSink sink = {};
  sink.User = &buf;
  sink.OnData = FetchOnData;
  HttpResult r = Http_Stream(arena, req, sink, abort);
  HttpResponse res = {};
  res.Status = r.Status;
  res.Error = r.Error;
  res.Body = StrBuf_View(&buf);
  return res;
}

// ---- provider-request (transport-level) retry policy -----------------------

HttpRetryPolicy HttpRetryPolicy_Default() {
  HttpRetryPolicy p = {};
  p.Enabled = false;
  p.MaxRetries = 2;
  p.BaseDelayMs = 500;
  p.MaxDelayMs = 8000;
  return p;
}

bool HttpStatus_IsRetryable(i32 status) {
  if (status == 408 || status == 409 || status == 429) return true;
  return status >= 500 && status < 600;
}

bool HttpRetry_ShouldRetry(const HttpRetryPolicy* policy, i32 status, bool transportFailure,
                           const HttpRetryMeta* meta, u32 attemptsMade) {
  if (!policy || !policy->Enabled) return false;
  if (attemptsMade >= policy->MaxRetries + 1) return false;  // no attempts remain
  if (meta && meta->HasShouldRetry) return meta->ShouldRetry;  // server signal wins
  if (transportFailure) return true;
  return HttpStatus_IsRetryable(status);
}

u32 HttpRetry_DelayMs(const HttpRetryPolicy* policy, const HttpRetryMeta* meta, u32 attemptIndex,
                     double jitterRandom01) {
  HttpRetryPolicy def = HttpRetryPolicy_Default();
  const HttpRetryPolicy* p = policy ? policy : &def;
  u32 maxDelay = p->MaxDelayMs ? p->MaxDelayMs : def.MaxDelayMs;

  if (meta && meta->RetryAfterMs >= 0) {
    u64 serverMs = (u64)meta->RetryAfterMs;
    if (serverMs > maxDelay) serverMs = maxDelay;
    return (u32)serverMs;
  }

  u32 base = p->BaseDelayMs ? p->BaseDelayMs : def.BaseDelayMs;
  u64 backoff = base;
  for (u32 i = 0; i < attemptIndex; i++) {
    backoff *= 2;
    if (backoff >= maxDelay) {
      backoff = maxDelay;
      break;
    }
  }
  if (backoff > maxDelay) backoff = maxDelay;

  if (jitterRandom01 < 0) jitterRandom01 = 0;
  if (jitterRandom01 > 1) jitterRandom01 = 1;
  double factor = 0.75 + jitterRandom01 * 0.5;  // +/-25% jitter
  double jittered = (double)backoff * factor;
  if (jittered < 0) jittered = 0;
  if (jittered > maxDelay) jittered = maxDelay;
  return (u32)(jittered + 0.5);
}

HttpResult Http_StreamWithRetryDeps(Arena* arena, const HttpRequest* req, HttpStreamSink sink,
                                    volatile i32* abort, const HttpRetryPolicy* policy,
                                    const HttpRetryDeps* deps) {
  u32 attempts = 0;
  HttpResult r = {};
  for (;;) {
    r = deps->Transport(deps->TransportUser, arena, req, sink, abort);
    attempts++;

    if (!policy || !policy->Enabled) return r;  // policy off -> single attempt, passthrough
    if (r.BodyStarted) return r;                // already forwarded success bytes -> never retry
    if (abort && Atomic_LoadI32(abort)) return r;              // user cancellation isn't masked by a retry

    bool transportFailure = r.Status < 0;
    if (!HttpRetry_ShouldRetry(policy, r.Status, transportFailure, &r.Meta, attempts)) return r;

    double jitter = deps->Rand ? deps->Rand(deps->RandUser) : 0.5;
    u32 delay = HttpRetry_DelayMs(policy, &r.Meta, attempts - 1, jitter);
    HttpRetryEvent event = {};
    event.Kind = HttpRetryEventKind::Start;
    event.Attempt = attempts;
    event.MaxRetries = policy->MaxRetries;
    event.DelayMs = delay;
    event.Status = r.Status;
    event.Error = r.Error.Length ? r.Error : r.Body;
    if (policy->OnEvent) policy->OnEvent(policy->EventUser, &event);

    u32 remaining = delay;
    while (remaining > 0) {
      if (abort && Atomic_LoadI32(abort)) break;
      u32 step = remaining < 100 ? remaining : 100;
      if (deps->Sleep) deps->Sleep(deps->SleepUser, step);
      remaining -= step;
    }
    event.Kind = HttpRetryEventKind::End;
    if (policy->OnEvent) policy->OnEvent(policy->EventUser, &event);
    if (abort && Atomic_LoadI32(abort)) {
      HttpResult aborted = {};
      aborted.Status = -1;
      aborted.Error = SVL("aborted");
      aborted.Meta.RetryAfterMs = -1;
      return aborted;
    }
  }
}

namespace {
HttpResult RealTransport(void*, Arena* arena, const HttpRequest* req, HttpStreamSink sink,
                         volatile i32* abort) {
  return Http_Stream(arena, req, sink, abort);
}
void RealSleep(void*, u32 ms) { Sleep_Ms(ms); }
double RealRand(void*) { return (double)rand() / ((double)RAND_MAX + 1.0); }
}  // namespace

HttpResult Http_StreamWithRetry(Arena* arena, const HttpRequest* req, HttpStreamSink sink,
                                volatile i32* abort, const HttpRetryPolicy* policy) {
  HttpRetryDeps deps = {};
  deps.Transport = RealTransport;
  deps.Sleep = RealSleep;
  deps.Rand = RealRand;
  return Http_StreamWithRetryDeps(arena, req, sink, abort, policy, &deps);
}
