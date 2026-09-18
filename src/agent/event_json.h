#pragma once

#include "agent/harness_event.h"
#include "base/str.h"

// Appends one JSON object (without a newline) for a harness event.
//
// Durable records are emitted verbatim through SessionRecord_Encode rather than
// re-flattened, so the stream carries exactly the authoritative state the
// session log holds and there is only one message encoder in the tree.
// Transient events get purpose-built shapes because they never reach the log.
//
// Structural stream markers succeed without changing out. Encoding failures
// return an error and also leave out unchanged; neither emits a partial object.
SessionResult AgentEventJson_Write(StrBuf* out, const AgentHarnessEvent* event);
void AgentEventJson_WriteResult(StrBuf* out, SessionResult result);
