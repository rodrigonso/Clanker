#pragma once
#include "base/arena.h"
#include "base/str.h"

// Providers deliver text at wildly different cadences: token-level deltas of a
// few characters every ~10-15ms, or coalesced bursts of tens of characters
// every ~80-100ms. Revealing each burst the instant it lands makes the
// transcript stair-step, so incoming deltas are copied into owned storage and
// handed to the display at a paced rate that decouples arrival from reveal.
//
// The pace is a proportional controller over the unrevealed backlog: the rate
// rises the further behind the display falls, so the backlog settles at a
// bounded depth rather than growing without limit. Pacing is display-only --
// nothing on the agent thread ever waits on it. See docs/UI.md.
struct StreamReveal {
  StrBuf Pending;
  u32 Offset;
  u64 LastTickMs;          // 0 until the first frame after text arrives
  u64 DeadlineMs;          // 0 when unarmed
  u32 CreditMilliScalars;  // reveal budget carried between frames
};

// Once a deadline is armed, everything pending at that moment is revealed
// within this many milliseconds, bounding how long a display event queued
// behind the reveal has to wait.
constexpr u64 kRevealDeadlineMs = 120;

void StreamReveal_Init(StreamReveal* r, Arena* arena);
void StreamReveal_Push(StreamReveal* r, StrView text);
void StreamReveal_Adopt(StreamReveal* r, StrBuf text);
bool StreamReveal_HasPending(const StreamReveal* r);

// Reveal this frame's share of the pending text. `blocked` says display work is
// queued behind the reveal and cannot be applied until it drains; it arms a
// one-shot deadline kRevealDeadlineMs out, after which everything pending is
// revealed at once, and lets malformed trailing bytes through rather than
// waiting for a continuation that will never arrive. Arming is idempotent, so
// staying blocked cannot push the tail further out, and neither can text pushed
// after the fact. The returned view is valid until the next push or adopt.
StrView StreamReveal_NextFrame(StreamReveal* r, u64 nowMs, bool blocked);
