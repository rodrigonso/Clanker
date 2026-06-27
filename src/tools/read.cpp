#include <stdio.h>

#include "json/json.h"
#include "tools/tools.h"

static const u32 READ_MAX_BYTES = 64 * 1024;

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

  StrBuf content = StrBuf_Make(arena, 8192);
  char buf[8192];
  size_t n;
  bool truncated = false;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    if (content.Length + (u32)n > READ_MAX_BYTES) {
      u32 room = READ_MAX_BYTES - content.Length;
      StrBuf_Append(&content, StrView{buf, room});
      truncated = true;
      break;
    }
    StrBuf_Append(&content, StrView{buf, (u32)n});
  }
  fclose(f);
  if (truncated) StrBuf_AppendCStr(&content, "\n... [truncated at 64KB]");

  out->Content = Array_Make<Content>(arena, 1);
  Array_Push(&out->Content, Content_Text(StrBuf_View(&content)));
  return true;
}

Tool ReadTool() {
  Tool t = {};
  t.Name = SVL("read");
  t.Label = SVL("Read");
  t.Description = SVL("Read a UTF-8 text file from disk. Returns up to 64KB of content.");
  t.ParamsJsonSchema = SVL(
      "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Path to "
      "the file to read\"}},\"required\":[\"path\"]}");
  t.Execute = ReadExec;
  return t;
}
