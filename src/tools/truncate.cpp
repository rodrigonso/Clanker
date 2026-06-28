#include "tools/truncate.h"

// Lines are separated by '\n'. A trailing '\n' terminates the last line but
// does not create an extra empty final line: "" has 0 lines, "\n" has 1 line.
static u32 Trunc_CountLines(StrView content) {
  if (content.Length == 0) return 0;

  u32 count = 0;
  for (u32 i = 0; i < content.Length; i++) {
    if (content.Data[i] == '\n') count++;
  }
  if (content.Data[content.Length - 1] != '\n') count++;
  return count;
}

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

static u32 Trunc_HeadByteEnd(StrView content, u32 end, u32 maxBytes) {
  if (end <= maxBytes) return end;
  if (maxBytes == 0) return 0;

  u32 result = 0;
  for (u32 i = 0; i < maxBytes; i++) {
    if (content.Data[i] == '\n') result = i + 1;
  }
  return result ? result : maxBytes;
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
  return minStart;
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
