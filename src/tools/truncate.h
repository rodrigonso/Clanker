#pragma once
#include "base/arena.h"
#include "base/str.h"

static const u32 TRUNC_MAX_LINES = 2000;
static const u32 TRUNC_MAX_BYTES = 50u * 1024u;  // 51200

struct TruncResult {
  StrView Content;     // possibly-truncated content (arena-owned if changed, else a slice of input)
  bool Truncated;
  bool ByBytes;        // when Truncated: true => byte limit hit; false => line limit hit
  u32 TotalLines;      // line count of the ORIGINAL content
  u32 TotalBytes;      // byte count of the ORIGINAL content
  u32 OutputLines;     // line count of the returned content
  u32 OutputBytes;     // byte count of the returned content
};

// Keep the FIRST maxLines lines AND at most maxBytes bytes (whichever limit is hit first).
TruncResult Trunc_Head(Arena* arena, StrView content, u32 maxLines, u32 maxBytes);
// Keep the LAST maxLines lines AND at most maxBytes bytes.
TruncResult Trunc_Tail(Arena* arena, StrView content, u32 maxLines, u32 maxBytes);
