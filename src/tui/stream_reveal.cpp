#include <string.h>

#include "tui/stream_reveal.h"

namespace {

const u32 COMPACT_AFTER_BYTES = 64 * 1024;

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
  ReclaimConsumed(r);
  StrBuf_Append(&r->Pending, text);
}

void StreamReveal_Adopt(StreamReveal* r, StrBuf text) {
  FFI(StreamReveal_HasPending(r));
  r->Pending = text;
  r->Offset = 0;
}

StrView StreamReveal_NextFrame(StreamReveal* r, u64 nowMs, bool final) {
  (void)nowMs;
  if (!StreamReveal_HasPending(r)) return StrView{};

  u32 start = r->Offset;
  u32 end = start;
  while (end < r->Pending.Length) {
    u32 scalar = Utf8ScalarBytes(r->Pending.Data + end, r->Pending.Length - end);
    if (scalar == 0) {
      if (!final) break;
      scalar = 1;
    }
    end += scalar;
  }

  if (end == start) return StrView{};
  r->Offset = end;
  return StrView{r->Pending.Data + start, end - start};
}
