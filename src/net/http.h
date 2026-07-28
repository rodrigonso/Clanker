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

struct HttpRequest {
  StrView Method;  // "POST" or "GET"
  StrView Url;
  const HttpHeader* Headers;
  u32 HeaderCount;
  StrView Body;  // request body (for POST)
  bool FollowRedirects;
};

struct HttpStreamSink {
  void* User;
  // Return false to abort the transfer.
  bool (*OnData)(void* user, StrView chunk);
};

struct HttpResult {
  i32 Status;     // HTTP status code, or -1 on transport error / abort
  StrView Error;  // transport error message (arena-owned) when Status < 0
};

// Non-streaming request: accumulates the full response body. For request/response
// endpoints like OAuth token exchange (not SSE).
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
