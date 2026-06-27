#include "llm/retry.h"

// Case-insensitive "does haystack contain needle" (needle is lowercase ASCII).
static bool ContainsCI(StrView hay, const char* needle) {
  u32 nl = 0;
  while (needle[nl]) nl++;
  if (nl == 0) return true;
  if (nl > hay.Length) return false;
  for (u32 i = 0; i + nl <= hay.Length; i++) {
    u32 k = 0;
    for (; k < nl; k++) {
      char c = hay.Data[i + k];
      if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
      if (c != needle[k]) break;
    }
    if (k == nl) return true;
  }
  return false;
}

static bool ContainsAny(StrView hay, const char* const* needles, u32 count) {
  for (u32 i = 0; i < count; i++)
    if (ContainsCI(hay, needles[i])) return true;
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
    "429",             "500",                "502",
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
