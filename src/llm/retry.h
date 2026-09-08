#pragma once
#include "base/str.h"

// Provider retry configuration is transport-independent data. Embedders may
// report request retries without linking Clanker's bundled HTTP transport.
enum class HttpRetryEventKind : u8 {
  Start,
  End,
};

struct HttpRetryEvent {
  HttpRetryEventKind Kind;
  u32 Attempt;  // 1 for the first retry
  u32 MaxRetries;
  u32 DelayMs;
  i32 Status;
  StrView Error;  // transport error or buffered non-success response body
};

typedef void (*HttpRetryEventFn)(void* user, const HttpRetryEvent* event);

struct HttpRetryPolicy {
  bool Enabled;  // default-off; caller opts in
  u32 MaxRetries;  // retry attempts after the first try
  u32 BaseDelayMs;
  u32 MaxDelayMs;  // caps server-provided delays as well as computed backoff
  void* EventUser;
  HttpRetryEventFn OnEvent;
};

// Enabled=false, MaxRetries=2, BaseDelayMs=500, MaxDelayMs=8000.
HttpRetryPolicy HttpRetryPolicy_Default();

// Classifies whether a failed provider error looks like a transient provider or
// transport error worth retrying. Port of pi's ai/utils/retry.ts
// isRetryableAssistantError: a curated set of retryable substrings (overloaded,
// rate limit, HTTP 408/409/429/5xx, network/transport, premature stream end)
// minus a non-retryable exclusion set (quota / billing / subscription usage
// limits).
//
// Matching is case-insensitive substring (pi uses regex; the patterns here are
// literal alternatives covering the same cases). Pass the assistant error text
// (clanker's Message.ErrorMessage, which already includes "HTTP <status>" and
// the response body). Returns false for empty input.
bool IsRetryableError(StrView errorMessage);
