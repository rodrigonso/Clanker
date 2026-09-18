#include "base/file_io.h"
#include "base/thread.h"
#include "crypto/base64.h"
#include "json/json.h"
#include "tools/fs.h"
#include "tools/imagemime.h"
#include "tools/tools.h"
#include "tools/truncate.h"

#include <string.h>

static const u32 READ_CHUNK_BYTES = 64u * 1024u;
static const u32 READ_SCRATCH_BYTES = 256u * 1024u;
// Clanker's resource policy, not a provider limit. Inline images are sent
// base64-encoded with every later request, and encoding expands this by 4/3
// (rounded up to a four-byte group); raw image data is never retained.
static const u32 READ_MAX_IMAGE_BYTES = 20u * 1024u * 1024u;

static void Read_AppendNoticePrefix(StrBuf* b) {
  if (b->Length > 0) StrBuf_AppendCStr(b, "\n\n");
}

static bool Read_Next(FileReader* file, char* buffer, u32 capacity, u32* count,
                      volatile i32* abort, const char** failure) {
  if (abort && Atomic_LoadI32(abort)) {
    *failure = "Operation aborted";
    return false;
  }
  FileIoResult read = File_ReadChunk(file, buffer, capacity, count);
  if (abort && Atomic_LoadI32(abort)) {
    *failure = "Operation aborted";
    return false;
  }
  if (read != FileIoResult::Success) {
    *failure = "failed to read file";
    return false;
  }
  return true;
}

static bool Read_Image(Arena* arena, Arena* scratch, FileReader* file,
                       char* buffer, u32 count, volatile i32* abort,
                       StrView* output, const char** failure) {
  usize mark = scratch->Used;
  char* encoded = (char*)ArenaAlloc(arena, 0, 1);
  u32 encodedBytes = 0;
  u32 total = count;
  for (;;) {
    u32 complete = count - count % 3u;
    StrView part = Base64Encode(scratch, (const byte*)buffer, complete);
    // Only encoded bytes are allocated in the result arena during this loop,
    // so extending the allocation never leaves abandoned growth buffers.
    memcpy(ArenaAlloc(arena, part.Length, 1), part.Data, part.Length);
    encodedBytes += part.Length;
    ArenaRestore(scratch, mark);
    u32 carry = count - complete;
    memmove(buffer, buffer + complete, carry);
    u32 next = 0;
    if (!Read_Next(file, buffer + carry, READ_CHUNK_BYTES - carry, &next,
                   abort, failure))
      return false;
    if (next > READ_MAX_IMAGE_BYTES - total) {
      *failure = "Image exceeds 20 MiB raw size limit";
      return false;
    }
    total += next;
    count = carry + next;
    if (!next) break;
  }
  StrView tail = Base64Encode(scratch, (const byte*)buffer, count);
  memcpy(ArenaAlloc(arena, tail.Length, 1), tail.Data, tail.Length);
  encodedBytes += tail.Length;
  *(char*)ArenaAlloc(arena, 1, 1) = '\0';
  *output = StrView{encoded, encodedBytes};
  return true;
}

struct ReadText {
  char* Prefix;
  u32 PrefixLength;
  u64 TotalBytes;
  u64 TotalLines;
  u64 SelectedBytes;
  u64 SelectedNewlines;
  u64 FirstLineBytes;
  u64 LineEnd;
  char Last;
  char SelectedLast;
};

static bool Read_TextChunk(ReadText* text, StrView chunk, u64 start, u64 limit) {
  if ((u64)chunk.Length > ~(u64)0 - text->TotalBytes) return false;
  text->TotalBytes += chunk.Length;
  if (chunk.Length) text->Last = chunk.Data[chunk.Length - 1];
  for (u32 pos = 0; pos < chunk.Length;) {
    const char* newline =
        (const char*)memchr(chunk.Data + pos, '\n', chunk.Length - pos);
    u32 end = newline ? (u32)(newline - chunk.Data) + 1u : chunk.Length;
    u32 length = end - pos;
    if (text->TotalLines >= start &&
        (!limit || text->TotalLines - start < limit)) {
      u32 keep = ClankerMin(length, TRUNC_MAX_BYTES + 1u - text->PrefixLength);
      memcpy(text->Prefix + text->PrefixLength, chunk.Data + pos, keep);
      text->PrefixLength += keep;
      text->SelectedBytes += length;
      text->SelectedLast = chunk.Data[end - 1];
      if (!text->SelectedNewlines)
        text->FirstLineBytes += length - (newline ? 1u : 0u);
      if (newline && ++text->SelectedNewlines == TRUNC_MAX_LINES)
        text->LineEnd = text->SelectedBytes;
    }
    if (newline) text->TotalLines++;
    pos = end;
  }
  return true;
}

static bool Read_Text(Arena* arena, Arena* scratch, FileReader* file, char* buffer,
                      u32 count, StrView path, const ToolArgs* args,
                      volatile i32* abort, StrView* output, StrBuf* err,
                      const char** failure) {
  i64 offset = ToolArgs_GetInt(args, "offset", 0);
  i64 requestedLimit = ToolArgs_GetInt(args, "limit", 0);
  u64 startLine0 = offset > 0 ? (u64)(offset - 1) : 0;
  u64 limit = requestedLimit > 0 ? (u64)requestedLimit : 0;
  ReadText text = {};
  text.Prefix = ArenaPushArray(scratch, char, TRUNC_MAX_BYTES + 1u);
  for (;;) {
    if (!Read_TextChunk(&text, StrView{buffer, count}, startLine0, limit)) {
      *failure = "file size exceeds 64-bit counter range";
      return false;
    }
    if (!Read_Next(file, buffer, READ_CHUNK_BYTES, &count, abort, failure))
      return false;
    if (!count) break;
  }
  if (text.TotalBytes && text.Last != '\n') text.TotalLines++;
  u64 totalLines = text.TotalLines;
  if (startLine0 >= totalLines && totalLines) {
    StrBuf_AppendCStr(err, "Offset ");
    StrBuf_AppendI64(err, offset);
    StrBuf_AppendCStr(err, " is beyond end of file (");
    StrBuf_AppendU64(err, totalLines);
    StrBuf_AppendCStr(err, " lines total)");
    return false;
  }

  // Selection removes just its final LF, not CR and not other trailing LFs.
  if (text.SelectedBytes && text.SelectedLast == '\n') text.SelectedBytes--;
  u32 kept = (u32)ClankerMin((u64)text.PrefixLength, text.SelectedBytes);
  TruncResult tr = Trunc_Head(scratch, StrView{text.Prefix, kept},
                            TRUNC_MAX_LINES, TRUNC_MAX_BYTES);
  u64 lineEnd = text.LineEnd ? ClankerMin(text.LineEnd, text.SelectedBytes)
                            : text.SelectedBytes;
  tr.Truncated = tr.Content.Length != text.SelectedBytes;
  tr.ByBytes = tr.Content.Length < lineEnd;

  StrBuf result = StrBuf_Make(arena, tr.Content.Length + 256 + path.Length);
  StrBuf_Append(&result, tr.Content);

  u64 startLine1 = startLine0 + 1;
  if (tr.Truncated) {
    Read_AppendNoticePrefix(&result);
    if (tr.ByBytes && tr.OutputLines <= 1 && text.FirstLineBytes > TRUNC_MAX_BYTES) {
      StrBuf_AppendCStr(&result, "[Line ");
      StrBuf_AppendU64(&result, startLine1);
      StrBuf_AppendCStr(&result, " is ");
      Trunc_AppendKb(&result, text.FirstLineBytes);
      StrBuf_AppendCStr(&result, ", exceeds ");
      Trunc_AppendKb(&result, TRUNC_MAX_BYTES);
      StrBuf_AppendCStr(&result, " limit. Use shell: sed -n '");
      StrBuf_AppendU64(&result, startLine1);
      StrBuf_AppendCStr(&result, "p' ");
      StrBuf_Append(&result, path);
      StrBuf_AppendCStr(&result, " | head -c 51200]");
    } else {
      StrBuf_AppendCStr(&result, "[Showing lines ");
      StrBuf_AppendU64(&result, startLine1);
      StrBuf_AppendChar(&result, '-');
      StrBuf_AppendU64(&result, startLine1 + tr.OutputLines - 1u);
      StrBuf_AppendCStr(&result, " of ");
      StrBuf_AppendU64(&result, totalLines);
      if (tr.ByBytes) {
        StrBuf_AppendCStr(&result, " (");
        Trunc_AppendKb(&result, TRUNC_MAX_BYTES);
        StrBuf_AppendCStr(&result, " limit)");
      }
      StrBuf_AppendCStr(&result, ". Use offset=");
      StrBuf_AppendU64(&result, startLine1 + tr.OutputLines);
      StrBuf_AppendCStr(&result, " to continue.]");
    }
  } else if (limit > 0 && startLine0 + limit < totalLines) {
    u64 shownEnd = startLine0 + limit;
    Read_AppendNoticePrefix(&result);
    StrBuf_AppendChar(&result, '[');
    StrBuf_AppendU64(&result, totalLines - shownEnd);
    StrBuf_AppendCStr(&result, " more lines in file. Use offset=");
    StrBuf_AppendU64(&result, shownEnd + 1u);
    StrBuf_AppendCStr(&result, " to continue.]");
  }

  *output = StrBuf_View(&result);
  return true;
}

static bool ReadExec(void* ctx, Arena* arena, StrView id, const ToolArgs* args,
                     volatile i32* abort, const ToolUpdateSink* updates,
                     ToolResult* out, StrBuf* err) {
  (void)ctx;
  (void)id;
  (void)updates;
  StrView path = ToolArgs_GetStr(args, "path");
  if (path.Length == 0) {
    StrBuf_AppendCStr(err, "missing 'path' argument");
    return false;
  }
  if (abort && Atomic_LoadI32(abort)) {
    StrBuf_AppendCStr(err, "Operation aborted");
    return false;
  }
  if (path.Length >= READ_SCRATCH_BYTES) {
    StrBuf_AppendCStr(err, "cannot open file: path is too long");
    return false;
  }

  // Includes the chunk, selected prefix and truncation/base64 scratch; this
  // reservation is a hard bound independent of file size.
  Arena scratch = ArenaCreate(READ_SCRATCH_BYTES);
  FileReader file = {};
  if (File_OpenRead(&scratch, path, &file) != FileIoResult::Success) {
    StrBuf_AppendCStr(err, "cannot open file: ");
    StrBuf_Append(err, path);
    ArenaRelease(&scratch);
    return false;
  }
  char* buffer = ArenaPushArray(&scratch, char, READ_CHUNK_BYTES);
  const char* failure = nullptr;
  u32 count = 0;
  StrView mime = {};
  StrView content = {};
  usize outputMark = arena->Used;
  bool ok = Read_Next(&file, buffer, 16, &count, abort, &failure);
  if (ok) {
    mime = Img_SniffMime((const byte*)buffer, count);
    if (!mime.Length) mime = Img_MimeForExt(Fs_Ext(path));
    ok = mime.Length
             ? Read_Image(arena, &scratch, &file, buffer, count, abort, &content, &failure)
             : Read_Text(arena, &scratch, &file, buffer, count, path, args, abort,
                         &content, err, &failure);
  }
  bool closed = File_Close(&file) == FileIoResult::Success;
  ArenaRelease(&scratch);
  if (!ok || !closed) {
    // Text argument errors have already populated err, so preserve its storage.
    if (failure || ok) {
      ArenaRestore(arena, outputMark);
      if (mime.Length) ArenaTrim(arena, outputMark);
    }
    if (failure) {
      StrBuf_AppendCStr(err, failure);
      StrBuf_AppendCStr(err, ": ");
      StrBuf_Append(err, path);
    }
    if (!closed) {
      if (err->Length) StrBuf_AppendCStr(err, "; ");
      StrBuf_AppendCStr(err, "failed to close file: ");
      StrBuf_Append(err, path);
    }
    return false;
  }

  out->Content = Array_Make<Content>(arena, mime.Length ? 2u : 1u);
  if (mime.Length) {
    StrBuf text = StrBuf_Make(arena, 32 + mime.Length);
    StrBuf_AppendCStr(&text, "Read image file [");
    StrBuf_Append(&text, mime);
    StrBuf_AppendChar(&text, ']');
    Array_Push(&out->Content, Content_Text(StrBuf_View(&text)));
    Array_Push(&out->Content, Content_Image(content, mime));
  } else {
    Array_Push(&out->Content, Content_Text(content));
  }
  return true;
}

constexpr StrView kReadPromptGuidelines[] = {
    SVL("Use read to examine files instead of cat or sed.")};

Tool ReadTool() {
  Tool t = {};
  t.Name = SVL("read");
  t.Label = SVL("Read");
  t.Description = SVL("Read a UTF-8 text file or image from disk.");
  t.PromptSnippet = SVL("Read file contents");
  Tool_SetPromptGuidelines(&t, kReadPromptGuidelines);
  t.ParamsJsonSchema = SVL(
      "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Path to "
      "the file to read (relative or absolute)\"},\"offset\":{\"type\":\"number\",\"description\":"
      "\"Line number to start reading from (1-indexed)\"},\"limit\":{\"type\":\"number\","
      "\"description\":\"Maximum number of lines to read\"}},\"required\":[\"path\"]}");
  t.Execute = ReadExec;
  t.PermissionEffect = ToolPermissionEffect::Inspection;
  return t;
}
