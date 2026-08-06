#pragma once

#include "agent/harness_event.h"
#include "base/str.h"

// Serializes harness watch events as newline-delimited JSON, one object per
// event, for `--mode json`.
//
// Durable records are emitted verbatim through SessionRecord_Encode rather than
// re-flattened, so the stream carries exactly the authoritative state the
// session log holds and there is only one message encoder in the tree.
// Transient events get purpose-built shapes because they never reach the log.
//
// Returns false when an event has no representation (structural stream markers
// such as text_start/text_end, which carry no payload a consumer can use); the
// caller should emit nothing in that case.
bool AgentEventJson_Write(StrBuf* out, const AgentHarnessEvent* event);
