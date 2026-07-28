#include <stdio.h>

#include "crypto/base64.h"
#include "json/json.h"
#include "tools/fs.h"
#include "tools/imagemime.h"
#include "tools/tools.h"
#include "tools/truncate.h"

static u32 Read_CountLines(StrView content) {
  if (content.Length == 0) return 0;
  u32 count = 0;
  for (u32 i = 0; i < content.Length; i++) {
    if (content.Data[i] == '\n') count++;
  }
  if (content.Data[content.Length - 1] != '\n') count++;
  return count;
}

static u32 Read_LineStart(StrView content, u32 line0) {
  if (line0 == 0) return 0;
  u32 line = 0;
  for (u32 i = 0; i < content.Length; i++) {
    if (content.Data[i] == '\n') {
      line++;
      if (line == line0) return i + 1;
    }
  }
  return content.Length;
}

static StrView Read_SelectLines(StrView content, u32 startLine0, u32 endLine0) {
  u32 start = Read_LineStart(content, startLine0);
  u32 end = Read_LineStart(content, endLine0);
  if (end > 0 && content.Data[end - 1] == '\n') end--;
  if (end < start) end = start;
  return StrView{content.Data + start, end - start};
}

static u32 Read_LineByteLen(StrView content, u32 line0) {
  u32 start = Read_LineStart(content, line0);
  u32 end = start;
  while (end < content.Length && content.Data[end] != '\n') end++;
  return end - start;
}

static void Read_AppendKb(StrBuf* b, u32 bytes) {
  u32 kbx10 = (bytes * 10u + 512u) / 1024u;
  StrBuf_AppendU64(b, kbx10 / 10u);
  StrBuf_AppendChar(b, '.');
  StrBuf_AppendU64(b, kbx10 % 10u);
  StrBuf_AppendCStr(b, "KB");
}

static void Read_AppendNoticePrefix(StrBuf* b) {
  if (b->Length > 0) StrBuf_AppendCStr(b, "\n\n");
}

static bool ReadExec(void* ctx, Arena* arena, StrView id, StrView argsJson, volatile i32* abort,
                     ToolResult* out, StrBuf* err) {
  (void)ctx;
  (void)id;
  (void)abort;
  JsonValue* j = Json_Parse(arena, argsJson);
  StrView path = Json_GetStr(j, "path");
  if (path.Length == 0) {
    StrBuf_AppendCStr(err, "missing 'path' argument");
    return false;
  }

  StrBuf p = StrBuf_Make(arena, path.Length + 1);
  StrBuf_Append(&p, path);
  FILE* f = fopen(StrBuf_CStr(&p), "rb");
  if (!f) {
    StrBuf_AppendCStr(err, "cannot open file: ");
    StrBuf_Append(err, path);
    return false;
  }

  StrBuf file = StrBuf_Make(arena, 8192);
  char buf[8192];
  size_t n = 0;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    StrBuf_Append(&file, StrView{buf, (u32)n});
  }
  fclose(f);

  StrView bytes = StrBuf_View(&file);
  u32 sniffLen = ClankMin(bytes.Length, 16u);
  StrView mime = Img_SniffMime((const byte*)bytes.Data, sniffLen);
  if (mime.Length == 0) mime = Img_MimeForExt(Fs_Ext(path));
  if (mime.Length != 0) {
    StrBuf text = StrBuf_Make(arena, 32 + mime.Length);
    StrBuf_AppendCStr(&text, "Read image file [");
    StrBuf_Append(&text, mime);
    StrBuf_AppendChar(&text, ']');

    out->Content = Array_Make<Content>(arena, 2);
    Array_Push(&out->Content, Content_Text(StrBuf_View(&text)));
    Array_Push(&out->Content,
               Content_Image(Base64Encode(arena, (const byte*)bytes.Data, bytes.Length), mime));
    return true;
  }

  i64 offset = Json_GetInt(j, "offset", 0);
  i64 limit = Json_GetInt(j, "limit", 0);
  u32 totalLines = Read_CountLines(bytes);
  u32 startLine0 = 0;
  if (offset > 0) {
    if ((u64)(offset - 1) >= (u64)totalLines && totalLines > 0) {
      StrBuf_AppendCStr(err, "Offset ");
      StrBuf_AppendI64(err, offset);
      StrBuf_AppendCStr(err, " is beyond end of file (");
      StrBuf_AppendU64(err, totalLines);
      StrBuf_AppendCStr(err, " lines total)");
      return false;
    }
    startLine0 = (u32)(offset - 1);
  }

  u32 endLine0 = totalLines;
  if (limit > 0) {
    u64 wantedEnd = (u64)startLine0 + (u64)limit;
    endLine0 = wantedEnd < (u64)totalLines ? (u32)wantedEnd : totalLines;
  }

  StrView selected = Read_SelectLines(bytes, startLine0, endLine0);
  TruncResult tr = Trunc_Head(arena, selected, TRUNC_MAX_LINES, TRUNC_MAX_BYTES);

  StrBuf result = StrBuf_Make(arena, tr.Content.Length + 256 + path.Length);
  StrBuf_Append(&result, tr.Content);

  u32 startLine1 = startLine0 + 1;
  if (tr.Truncated && !tr.ByBytes) {
    Read_AppendNoticePrefix(&result);
    StrBuf_AppendCStr(&result, "[Showing lines ");
    StrBuf_AppendU64(&result, startLine1);
    StrBuf_AppendChar(&result, '-');
    StrBuf_AppendU64(&result, startLine1 + tr.OutputLines - 1u);
    StrBuf_AppendCStr(&result, " of ");
    StrBuf_AppendU64(&result, totalLines);
    StrBuf_AppendCStr(&result, ". Use offset=");
    StrBuf_AppendU64(&result, startLine1 + tr.OutputLines);
    StrBuf_AppendCStr(&result, " to continue.]");
  } else if (tr.Truncated && tr.ByBytes) {
    u32 firstLineBytes = Read_LineByteLen(bytes, startLine0);
    if (tr.OutputLines <= 1 && firstLineBytes > TRUNC_MAX_BYTES) {
      Read_AppendNoticePrefix(&result);
      StrBuf_AppendCStr(&result, "[Line ");
      StrBuf_AppendU64(&result, startLine1);
      StrBuf_AppendCStr(&result, " is ");
      Read_AppendKb(&result, firstLineBytes);
      StrBuf_AppendCStr(&result, ", exceeds ");
      Read_AppendKb(&result, TRUNC_MAX_BYTES);
      StrBuf_AppendCStr(&result, " limit. Use shell: sed -n '");
      StrBuf_AppendU64(&result, startLine1);
      StrBuf_AppendCStr(&result, "p' ");
      StrBuf_Append(&result, path);
      StrBuf_AppendCStr(&result, " | head -c 51200]");
    } else {
      Read_AppendNoticePrefix(&result);
      StrBuf_AppendCStr(&result, "[Showing lines ");
      StrBuf_AppendU64(&result, startLine1);
      StrBuf_AppendChar(&result, '-');
      StrBuf_AppendU64(&result, startLine1 + tr.OutputLines - 1u);
      StrBuf_AppendCStr(&result, " of ");
      StrBuf_AppendU64(&result, totalLines);
      StrBuf_AppendCStr(&result, " (");
      Read_AppendKb(&result, TRUNC_MAX_BYTES);
      StrBuf_AppendCStr(&result, " limit). Use offset=");
      StrBuf_AppendU64(&result, startLine1 + tr.OutputLines);
      StrBuf_AppendCStr(&result, " to continue.]");
    }
  } else if (limit > 0 && (u64)startLine0 + (u64)limit < (u64)totalLines) {
    u64 shownEnd = (u64)startLine0 + (u64)limit;
    Read_AppendNoticePrefix(&result);
    StrBuf_AppendChar(&result, '[');
    StrBuf_AppendU64(&result, (u64)totalLines - shownEnd);
    StrBuf_AppendCStr(&result, " more lines in file. Use offset=");
    StrBuf_AppendU64(&result, shownEnd + 1u);
    StrBuf_AppendCStr(&result, " to continue.]");
  }

  out->Content = Array_Make<Content>(arena, 1);
  Array_Push(&out->Content, Content_Text(StrBuf_View(&result)));
  return true;
}

Tool ReadTool() {
  Tool t = {};
  t.Name = SVL("read");
  t.Label = SVL("Read");
  t.Description = SVL("Read a UTF-8 text file or image from disk.");
  t.ParamsJsonSchema = SVL(
      "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Path to "
      "the file to read (relative or absolute)\"},\"offset\":{\"type\":\"number\",\"description\":"
      "\"Line number to start reading from (1-indexed)\"},\"limit\":{\"type\":\"number\","
      "\"description\":\"Maximum number of lines to read\"}},\"required\":[\"path\"]}");
  t.ReplayPolicy = ToolReplayPolicy::ReplaySafe;
  t.Execute = ReadExec;
  return t;
}
