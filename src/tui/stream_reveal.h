#pragma once
#include "base/arena.h"
#include "base/str.h"

// App-specific smoothing for bursty provider text. Incoming deltas are copied into
// owned storage, then exposed as rate-limited UTF-8-safe slices.
struct StreamReveal {
  StrBuf Pending;
  u32 Offset;
  u64 LastTickMs;
  u32 CreditMilliScalars;
};

void StreamReveal_Init(StreamReveal* r, Arena* arena);
void StreamReveal_Push(StreamReveal* r, StrView text);
void StreamReveal_Adopt(StreamReveal* r, StrBuf text);
bool StreamReveal_HasPending(const StreamReveal* r);

// Advance one display frame. When `final` is true, an incomplete trailing UTF-8
// scalar is treated as malformed input and paced byte-by-byte. The returned
// view remains valid until the next push or adopt.
StrView StreamReveal_NextFrame(StreamReveal* r, u64 nowMs, bool final);
