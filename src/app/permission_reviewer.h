#pragma once

#include "agent/permissions.h"
#include "net/http.h"

bool TypeSafePermissionReviewer_ModelSupported(StrView model);
bool TypeSafePermissionReviewer_ThresholdIsValid(f64 threshold);
// Optional test transport dependencies must outlive the returned reviewer.
AgentPermissionReviewer TypeSafePermissionReviewer_Make(
    Arena* arena, StrView model,
    f64 threshold = AGENT_PERMISSION_DEFAULT_REVIEWER_THRESHOLD,
    StrView authPath = {}, const HttpRetryDeps* deps = nullptr);
bool TypeSafePermissionReviewer_KeyConfigured(StrView authPath);

// The same request path with injectable transport, backoff, and randomness.
bool TypeSafePermissionReviewer_ReviewWithDeps(
    Arena* arena, StrView model, StrView apiKey,
    const AgentPermissionReviewRequest* request, volatile i32* abort,
    const HttpRetryDeps* deps, AgentPermissionReviewResult* out,
    f64 threshold = AGENT_PERMISSION_DEFAULT_REVIEWER_THRESHOLD);
