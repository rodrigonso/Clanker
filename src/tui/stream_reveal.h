#pragma once
#include "base/arena.h"
#include "base/str.h"

// Incoming provider deltas are copied into owned storage, then coalesced into
// UTF-8-safe render-frame slices without adding a typing-speed delay.
struct StreamReveal {
  StrBuf Pending;
  u32 Offset;
};

void StreamReveal_Init(StreamReveal* r, Arena* arena);
void StreamReveal_Push(StreamReveal* r, StrView text);
void StreamReveal_Adopt(StreamReveal* r, StrBuf text);
bool StreamReveal_HasPending(const StreamReveal* r);

// Drain all complete text currently available on the next display frame. When
// `final` is true, malformed trailing bytes are included so the display always
// makes progress. The returned view remains valid until the next push or adopt.
StrView StreamReveal_NextFrame(StreamReveal* r, u64 nowMs, bool final);
