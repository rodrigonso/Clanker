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
// (clank's Message.ErrorMessage, which already includes "HTTP <status>" and
// the response body). Returns false for empty input.
bool IsRetryableError(StrView errorMessage);

// =============================================================================
// Assistant-turn retry primitives (message-level, text-based). Distinct from
// the transport-level HTTP retry in net/http.h: this operates on a completed
// assistant Message after a full turn has failed, and is meant to be wired by
// the app/harness (default-off there; see agent/primitives.h AgentRetryPolicy,
// which already uses IsRetryableError below this layer). Pure helpers so the
// policy is testable without a provider or event plumbing.
// =============================================================================

// Plain exponential backoff (no jitter) for retrying a whole assistant turn.
// attemptIndex is 0 for the delay before the first retry, 1 before the
// second, etc. Doubles from baseDelayMs, capped at maxDelayMs.
u32 AssistantRetry_DelayMs(u32 baseDelayMs, u32 maxDelayMs, u32 attemptIndex);

// True when a failed assistant turn is worth resubmitting: the error text
// looks transient (IsRetryableError) AND the turn produced no usable content
// yet. Retrying a turn that already streamed real content would duplicate or
// discard that output, so any usable partial content always wins over a
// retry.
bool AssistantRetry_ShouldRetry(StrView errorMessage, bool hasUsableContent);
