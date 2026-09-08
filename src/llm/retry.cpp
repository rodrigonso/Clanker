#include "llm/retry.h"

HttpRetryPolicy HttpRetryPolicy_Default() {
  HttpRetryPolicy policy = {};
  policy.Enabled = false;
  policy.MaxRetries = 2;
  policy.BaseDelayMs = 500;
  policy.MaxDelayMs = 8000;
  return policy;
}

static bool ContainsAny(StrView hay, const char* const* needles, u32 count) {
  for (u32 i = 0; i < count; i++)
    if (StrView_ContainsInsensitiveAscii(hay, SV(needles[i]))) return true;
  return false;
}

// Subscription/account/quota limits returned as 429s but NOT transient throttles.
static const char* const NON_RETRYABLE[] = {
    "gousagelimiterror", "freeusagelimiterror", "monthly usage limit reached",
    "available balance",  "insufficient_quota", "out of budget",
    "quota exceeded",     "billing",            "usage limit",
};

// Transient provider load, HTTP status, transport, and premature stream-end text.
static const char* const RETRYABLE[] = {
    "overloaded",      "rate limit",         "rate-limit",
    "rate_limit",      "ratelimit",          "too many requests",
    "408",             "409",                "429",
    "500",             "502",
    "503",             "504",                "service unavailable",
    "service_unavailable", "server error",   "internal error",
    "provider returned error",               "network error",
    "connection error", "connection refused", "connection lost",
    "other side closed", "fetch failed",     "upstream connect",
    "reset before headers", "socket hang up", "timed out",
    "timeout",         "terminated",         "websocket closed",
    "websocket error", "ended without",      "stream ended before message_stop",
    "you can retry",   "try your request again", "please retry",
};

bool IsRetryableError(StrView errorMessage) {
  if (errorMessage.Length == 0) return false;
  if (ContainsAny(errorMessage, NON_RETRYABLE, (u32)(sizeof(NON_RETRYABLE) / sizeof(NON_RETRYABLE[0]))))
    return false;
  return ContainsAny(errorMessage, RETRYABLE, (u32)(sizeof(RETRYABLE) / sizeof(RETRYABLE[0])));
}
