#include <string.h>

#include "tui/stream_reveal.h"

namespace {

const u32 COMPACT_AFTER_BYTES = 64 * 1024;

// Reveal rate in scalars/second: a floor when nothing is waiting, plus a gain
// per backlogged byte. The gain is what bounds the queue -- text arriving at R
// scalars/second settles (R - BASE) / GAIN bytes behind, whatever its length.
const u32 BASE_SCALARS_PER_SECOND = 180;
const u32 BACKLOG_GAIN_PER_BYTE = 3;
const u32 MAX_SCALARS_PER_SECOND = 960;

// Elapsed time credited to one frame. Clamping time rather than scalars is
// deliberate: a per-frame scalar cap silently becomes a hard ceiling of
// cap x frame rate that no amount of backlog can overcome, which is how the
// previous pacing attempt throttled long replies.
const u64 MAX_STEP_MS = 50;

const u32 ONE_SCALAR = 1000;  // milli-scalar fixed point; no floating point here

// Return one complete UTF-8 scalar's byte length. A zero result means the
// buffer ends with a potentially valid but incomplete scalar. Invalid input
// consumes one byte so arbitrary provider output always makes progress.
u32 Utf8ScalarBytes(const char* data, u32 length) {
  if (length == 0) return 0;
  u8 lead = (u8)data[0];
  if (lead < 0x80) return 1;

  u32 need = 0;
  if (lead >= 0xC2 && lead <= 0xDF)
    need = 2;
  else if (lead >= 0xE0 && lead <= 0xEF)
    need = 3;
  else if (lead >= 0xF0 && lead <= 0xF4)
    need = 4;
  else
    return 1;

  if (length < need) return 0;
  for (u32 i = 1; i < need; i++) {
    if (((u8)data[i] & 0xC0) != 0x80) return 1;
  }

  // Reject overlong encodings, UTF-16 surrogates and values above U+10FFFF.
  if (need == 3 && lead == 0xE0 && (u8)data[1] < 0xA0) return 1;
  if (need == 3 && lead == 0xED && (u8)data[1] >= 0xA0) return 1;
  if (need == 4 && lead == 0xF0 && (u8)data[1] < 0x90) return 1;
  if (need == 4 && lead == 0xF4 && (u8)data[1] >= 0x90) return 1;
  return need;
}

void ReclaimConsumed(StreamReveal* r) {
  if (r->Offset == 0) return;
  if (r->Offset == r->Pending.Length) {
    StrBuf_Clear(&r->Pending);
    r->Offset = 0;
    return;
  }
  if (r->Offset < COMPACT_AFTER_BYTES || r->Offset < r->Pending.Length / 2) return;
  u32 remaining = r->Pending.Length - r->Offset;
  memmove(r->Pending.Data, r->Pending.Data + r->Offset, remaining);
  r->Pending.Length = remaining;
  r->Pending.Data[remaining] = '\0';
  r->Offset = 0;
}

u32 Backlog(const StreamReveal* r) { return r->Pending.Length - r->Offset; }

void ResetPacing(StreamReveal* r) {
  r->LastTickMs = 0;
  r->DeadlineMs = 0;
  // Seeded with one scalar so the first frame after text arrives is never
  // empty, however small the interval that frame is credited with.
  r->CreditMilliScalars = ONE_SCALAR;
}

}  // namespace

void StreamReveal_Init(StreamReveal* r, Arena* arena) {
  *r = {};
  r->Pending = StrBuf_Make(arena, 256);
}

bool StreamReveal_HasPending(const StreamReveal* r) {
  return r->Offset < r->Pending.Length;
}

void StreamReveal_Push(StreamReveal* r, StrView text) {
  if (text.Length == 0) return;
  bool wasEmpty = !StreamReveal_HasPending(r);
  ReclaimConsumed(r);
  StrBuf_Append(&r->Pending, text);
  // Only a transition out of empty reseeds the clock. Text arriving behind an
  // armed deadline joins the existing tail instead of starting a new one.
  if (wasEmpty) ResetPacing(r);
}

void StreamReveal_Adopt(StreamReveal* r, StrBuf text) {
  FFI(StreamReveal_HasPending(r));
  r->Pending = text;
  r->Offset = 0;
  ResetPacing(r);
}

StrView StreamReveal_NextFrame(StreamReveal* r, u64 nowMs, bool blocked) {
  if (!StreamReveal_HasPending(r)) {
    r->DeadlineMs = 0;
    return StrView{};
  }
  if (blocked && r->DeadlineMs == 0) {
    r->DeadlineMs = nowMs + kRevealDeadlineMs;
    FFI(r->DeadlineMs == 0);  // 0 is the unarmed sentinel
  }

  if (r->LastTickMs == 0) r->LastTickMs = nowMs;
  // A clock that moved backwards credits nothing rather than wrapping.
  u64 elapsed = nowMs > r->LastTickMs ? nowMs - r->LastTickMs : 0;
  if (elapsed > MAX_STEP_MS) elapsed = MAX_STEP_MS;
  r->LastTickMs = nowMs;

  u32 backlog = Backlog(r);
  u32 budget = 0;
  if (r->DeadlineMs != 0 && nowMs >= r->DeadlineMs) {
    budget = backlog;
  } else {
    // The backlog term counts bytes, not decoded scalars, to keep it O(1).
    // Multibyte text therefore overstates how far behind the display is, which
    // only reveals faster -- the safe direction.
    u64 rate = (u64)BASE_SCALARS_PER_SECOND + (u64)backlog * BACKLOG_GAIN_PER_BYTE;
    if (rate > MAX_SCALARS_PER_SECOND) rate = MAX_SCALARS_PER_SECOND;
    // Capping the balance at one full step keeps idle time from banking credit
    // that would later burst; it also bounds a frame at MAX_STEP_MS x rate.
    u64 credit = (u64)r->CreditMilliScalars + elapsed * rate;
    r->CreditMilliScalars = (u32)ClankMin(credit, MAX_STEP_MS * rate);
    budget = r->CreditMilliScalars / ONE_SCALAR;

    if (r->DeadlineMs != 0) {
      // This frame's share of the remaining backlog. Revealing backlog*e/T each
      // frame leaves the ratio backlog/remaining unchanged, so the deadline is
      // met by construction rather than by the drain above.
      u64 remaining = r->DeadlineMs - nowMs;
      u64 share = ((u64)backlog * elapsed + remaining - 1) / remaining;
      if (share > backlog) share = backlog;
      if (share > budget) budget = (u32)share;
    }
  }

  u32 start = r->Offset;
  u32 end = start;
  u32 consumed = 0;
  while (end < r->Pending.Length && consumed < budget) {
    u32 scalar = Utf8ScalarBytes(r->Pending.Data + end, r->Pending.Length - end);
    if (scalar == 0) {
      if (!blocked) break;
      scalar = 1;
    }
    end += scalar;
    consumed++;
  }

  if (end == start) return StrView{};
  u64 spent = (u64)consumed * ONE_SCALAR;
  r->CreditMilliScalars =
      spent >= r->CreditMilliScalars ? 0 : (u32)(r->CreditMilliScalars - spent);
  r->Offset = end;
  if (!StreamReveal_HasPending(r)) r->DeadlineMs = 0;
  return StrView{r->Pending.Data + start, end - start};
}
