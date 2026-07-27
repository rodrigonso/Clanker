#include <string.h>

#include "term/stream_reveal.h"

namespace {

const u32 COMPACT_AFTER_BYTES = 64 * 1024;
const u32 BASE_SCALARS_PER_SECOND = 120;
const u32 MEDIUM_SCALARS_PER_SECOND = 180;
const u32 HIGH_SCALARS_PER_SECOND = 240;
const u32 MEDIUM_BACKLOG_BYTES = 256;
const u32 HIGH_BACKLOG_BYTES = 1024;
const u32 MAX_SCALARS_PER_FRAME = 4;
const u32 ONE_SCALAR_CREDIT = 1000;

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
  if (wasEmpty) {
    r->LastTickMs = 0;
    r->CreditMilliScalars = ONE_SCALAR_CREDIT;
  }
}

void StreamReveal_Adopt(StreamReveal* r, StrBuf text) {
  FFI(StreamReveal_HasPending(r));
  r->Pending = text;
  r->Offset = 0;
  r->LastTickMs = 0;
  r->CreditMilliScalars = ONE_SCALAR_CREDIT;
}

StrView StreamReveal_NextFrame(StreamReveal* r, u64 nowMs, bool final) {
  u32 backlog = r->Pending.Length - r->Offset;
  if (backlog == 0) return StrView{};

  u32 rate = BASE_SCALARS_PER_SECOND;
  if (backlog >= HIGH_BACKLOG_BYTES)
    rate = HIGH_SCALARS_PER_SECOND;
  else if (backlog >= MEDIUM_BACKLOG_BYTES)
    rate = MEDIUM_SCALARS_PER_SECOND;

  if (r->LastTickMs == 0) {
    r->LastTickMs = nowMs;
  } else if (nowMs > r->LastTickMs) {
    u64 elapsed = nowMs - r->LastTickMs;
    if (elapsed > 100) elapsed = 100;
    u64 credit = (u64)r->CreditMilliScalars + elapsed * rate;
    u32 cap = MAX_SCALARS_PER_FRAME * ONE_SCALAR_CREDIT;
    r->CreditMilliScalars = (u32)(credit > cap ? cap : credit);
    r->LastTickMs = nowMs;
  }

  u32 budget = r->CreditMilliScalars / ONE_SCALAR_CREDIT;
  if (budget == 0) return StrView{};

  u32 start = r->Offset;
  u32 end = start;
  u32 consumed = 0;
  while (end < r->Pending.Length && consumed < budget) {
    u32 scalar = Utf8ScalarBytes(r->Pending.Data + end, r->Pending.Length - end);
    if (scalar == 0) {
      if (!final) break;
      scalar = 1;
    }
    end += scalar;
    consumed++;
  }

  if (end == start) return StrView{};
  r->CreditMilliScalars -= consumed * ONE_SCALAR_CREDIT;
  r->Offset = end;
  return StrView{r->Pending.Data + start, end - start};
}
