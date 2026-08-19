#pragma once
#include "base/str.h"

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
