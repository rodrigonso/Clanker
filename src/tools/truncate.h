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

// Lines are separated by '\n'. A trailing '\n' terminates the last line but
// does not create an extra empty final line: "" has 0 lines, "\n" has 1 line.
inline u32 Trunc_CountLines(StrView content) {
  if (content.Length == 0) return 0;
  u32 count = 0;
  for (u32 i = 0; i < content.Length; i++) {
    if (content.Data[i] == '\n') count++;
  }
  if (content.Data[content.Length - 1] != '\n') count++;
  return count;
}

// Size for a truncation notice, rendered with one decimal ("12.5KB").
inline void Trunc_AppendKb(StrBuf* b, u32 bytes) {
  u32 kbx10 = (bytes * 10u + 512u) / 1024u;
  StrBuf_AppendU64(b, kbx10 / 10u);
  StrBuf_AppendChar(b, '.');
  StrBuf_AppendU64(b, kbx10 % 10u);
  StrBuf_AppendCStr(b, "KB");
}

// Keep the FIRST maxLines lines AND at most maxBytes bytes (whichever limit is hit first).
TruncResult Trunc_Head(Arena* arena, StrView content, u32 maxLines, u32 maxBytes);
// Keep the LAST maxLines lines AND at most maxBytes bytes.
TruncResult Trunc_Tail(Arena* arena, StrView content, u32 maxLines, u32 maxBytes);
