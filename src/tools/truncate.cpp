#include "tools/truncate.h"

static StrView Trunc_Slice(StrView content, u32 start, u32 end) {
  if (end < start) end = start;
  if (start > content.Length) start = content.Length;
  if (end > content.Length) end = content.Length;
  return StrView{content.Data + start, end - start};
}

static u32 Trunc_HeadLineEnd(StrView content, u32 maxLines) {
  if (maxLines == 0) return 0;

  u32 lines = 0;
  for (u32 i = 0; i < content.Length; i++) {
    if (content.Data[i] == '\n') {
      lines++;
      if (lines == maxLines) return i + 1;
    }
  }
  return content.Length;
}

static u32 Trunc_TailLineStart(StrView content, u32 totalLines, u32 maxLines) {
  if (maxLines == 0) return content.Length;
  if (totalLines <= maxLines) return 0;

  u32 skipLines = totalLines - maxLines;
  u32 skipped = 0;
  for (u32 i = 0; i < content.Length; i++) {
    if (content.Data[i] == '\n') {
      skipped++;
      if (skipped == skipLines) return i + 1;
    }
  }
  return content.Length;
}

static bool Trunc_IsUtf8Continuation(char c) { return ((u8)c & 0xC0) == 0x80; }

// A byte cut that did not land on a newline can fall inside a multi-byte
// character and leave invalid UTF-8 that the provider rejects outright. These
// move the cut to the nearest character boundary: back to the lead byte when
// trimming a head, forward past the orphaned tail when trimming a tail. A
// character is at most 4 bytes, so walking more than 3 means the input was
// already invalid -- leave it for the serializer to replace.
static u32 Trunc_SnapBack(StrView content, u32 offset) {
  if (offset >= content.Length) return offset;
  u32 limit = offset > 3 ? offset - 3 : 0;
  while (offset > limit && Trunc_IsUtf8Continuation(content.Data[offset])) offset--;
  return offset;
}

static u32 Trunc_SnapForward(StrView content, u32 offset) {
  u32 limit = offset + 3 < content.Length ? offset + 3 : content.Length;
  while (offset < limit && Trunc_IsUtf8Continuation(content.Data[offset])) offset++;
  return offset;
}

static u32 Trunc_HeadByteEnd(StrView content, u32 end, u32 maxBytes) {
  if (end <= maxBytes) return end;
  if (maxBytes == 0) return 0;

  u32 result = 0;
  for (u32 i = 0; i < maxBytes; i++) {
    if (content.Data[i] == '\n') result = i + 1;
  }
  return result ? result : Trunc_SnapBack(content, maxBytes);
}

static u32 Trunc_TailByteStart(StrView content, u32 start, u32 maxBytes) {
  u32 selectionBytes = content.Length - start;
  if (selectionBytes <= maxBytes) return start;
  if (maxBytes == 0) return content.Length;

  u32 minStart = content.Length - maxBytes;
  if (minStart == 0 || content.Data[minStart - 1] == '\n') return minStart;

  for (u32 i = minStart; i < content.Length; i++) {
    if (content.Data[i] == '\n' && i + 1 < content.Length) return i + 1;
  }
  return Trunc_SnapForward(content, minStart);
}

static TruncResult Trunc_MakeResult(Arena* arena, StrView content, u32 start, u32 end, bool byBytes) {
  TruncResult result = {};
  result.TotalLines = Trunc_CountLines(content);
  result.TotalBytes = content.Length;

  result.Truncated = start != 0 || end != content.Length;
  result.ByBytes = result.Truncated && byBytes;

  if (result.Truncated) {
    result.Content = StrView_Dup(arena, Trunc_Slice(content, start, end));
  } else {
    result.Content = content;
  }

  result.OutputLines = Trunc_CountLines(result.Content);
  result.OutputBytes = result.Content.Length;
  return result;
}

TruncResult Trunc_Head(Arena* arena, StrView content, u32 maxLines, u32 maxBytes) {
  u32 lineEnd = Trunc_HeadLineEnd(content, maxLines);
  u32 byteEnd = Trunc_HeadByteEnd(content, lineEnd, maxBytes);
  bool byBytes = byteEnd < lineEnd;
  return Trunc_MakeResult(arena, content, 0, byteEnd, byBytes);
}

TruncResult Trunc_Tail(Arena* arena, StrView content, u32 maxLines, u32 maxBytes) {
  u32 totalLines = Trunc_CountLines(content);
  u32 lineStart = Trunc_TailLineStart(content, totalLines, maxLines);
  u32 byteStart = Trunc_TailByteStart(content, lineStart, maxBytes);
  bool byBytes = byteStart > lineStart;
  return Trunc_MakeResult(arena, content, byteStart, content.Length, byBytes);
}
