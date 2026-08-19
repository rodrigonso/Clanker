#include <stdlib.h>

#include "json/json.h"
#include "tools/fs.h"
#include "tools/tools.h"
#include "tools/truncate.h"

static int Ls_CompareEntries(const void* a, const void* b) {
  const FsEntry* ea = (const FsEntry*)a;
  const FsEntry* eb = (const FsEntry*)b;
  u32 n = ClankerMin(ea->Name.Length, eb->Name.Length);
  for (u32 i = 0; i < n; i++) {
    char ca = Ascii_ToLower(ea->Name.Data[i]);
    char cb = Ascii_ToLower(eb->Name.Data[i]);
    if (ca < cb) return -1;
    if (ca > cb) return 1;
  }
  if (ea->Name.Length < eb->Name.Length) return -1;
  if (ea->Name.Length > eb->Name.Length) return 1;
  return 0;
}

bool LsExec(void* ctx, Arena* arena, StrView id, const ToolArgs* args,
            volatile i32* abort, const ToolUpdateSink* updates,
            ToolResult* out, StrBuf* err) {
  (void)ctx;
  (void)id;
  (void)abort;
  (void)updates;
  StrView path = ToolArgs_GetStr(args, "path");
  if (path.Length == 0) path = SVL(".");
  i64 limit = ToolArgs_GetInt(args, "limit", 500);
  if (limit <= 0) limit = 500;

  if (!Fs_Exists(path)) {
    StrBuf_AppendCStr(err, "Path not found: ");
    StrBuf_Append(err, path);
    return false;
  }
  if (!Fs_IsDir(path)) {
    StrBuf_AppendCStr(err, "Not a directory: ");
    StrBuf_Append(err, path);
    return false;
  }

  bool ok = false;
  Array<FsEntry> entries = Fs_ListDir(arena, path, &ok);
  if (!ok) {
    StrBuf_AppendCStr(err, "Cannot list directory: ");
    StrBuf_Append(err, path);
    return false;
  }

  qsort(entries.Data, entries.Count, sizeof(FsEntry), Ls_CompareEntries);

  StrBuf raw = StrBuf_Make(arena, 4096);
  u32 emitCount = entries.Count;
  if (limit < (i64)emitCount) emitCount = (u32)limit;
  for (u32 i = 0; i < emitCount; i++) {
    StrBuf_Append(&raw, entries.Data[i].Name);
    if (entries.Data[i].IsDir) StrBuf_AppendChar(&raw, '/');
    StrBuf_AppendChar(&raw, '\n');
  }
  if (entries.Count == 0) StrBuf_AppendCStr(&raw, "(empty directory)");

  TruncResult tr = Trunc_Head(arena, StrBuf_View(&raw), 0xFFFFFFFFu, TRUNC_MAX_BYTES);
  StrBuf result = StrBuf_Make(arena, tr.Content.Length + 128);
  StrBuf_Append(&result, tr.Content);

  bool limitReached = (limit < (i64)entries.Count);
  bool byteLimitReached = tr.Truncated && tr.ByBytes;
  if (limitReached || byteLimitReached) {
    if (result.Length > 0 && result.Data[result.Length - 1] != '\n') StrBuf_AppendChar(&result, '\n');
    StrBuf_AppendChar(&result, '[');
    if (limitReached) {
      StrBuf_AppendI64(&result, limit);
      StrBuf_AppendCStr(&result, " entries limit reached. Use limit=");
      StrBuf_AppendU64(&result, (u64)limit * 2u);
      StrBuf_AppendCStr(&result, " for more.");
    }
    if (byteLimitReached) {
      if (limitReached) StrBuf_AppendChar(&result, ' ');
      Trunc_AppendKb(&result, TRUNC_MAX_BYTES);
      StrBuf_AppendCStr(&result, " limit reached");
    }
    StrBuf_AppendChar(&result, ']');
  }

  out->Content = Array_Make<Content>(arena, 1);
  Array_Push(&out->Content, Content_Text(StrBuf_View(&result)));
  return true;
}

Tool LsTool() {
  Tool t = {};
  t.Name = SVL("ls");
  t.Label = SVL("List");
  t.Description = SVL("List files and directories in a directory.");
  t.PromptSnippet = SVL("List directory contents");
  t.ParamsJsonSchema = SVL(
      "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Directory "
      "to list (default: current directory)\"},\"limit\":{\"type\":\"number\",\"description\":"
      "\"Maximum number of entries to return (default: 500)\"}}}");
  t.Execute = LsExec;
  return t;
}
