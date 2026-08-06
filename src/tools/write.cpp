#include <stdio.h>

#include "json/json.h"
#include "tools/filelock.h"
#include "tools/fs.h"
#include "tools/tools.h"

static bool Write_DoLocked(Arena* arena, StrView path, StrView content, volatile i32* abort,
                           ToolResult* out, StrBuf* err) {
  if (abort && *abort) {
    StrBuf_AppendCStr(err, "aborted");
    return false;
  }

  StrView dir = Fs_Parent(path);
  if (dir.Length != 0 && !Fs_MkdirP(arena, dir)) {
    StrBuf_AppendCStr(err, "failed to create parent directory for: ");
    StrBuf_Append(err, path);
    return false;
  }

  StrBuf p = StrBuf_Make(arena, path.Length + 1);
  StrBuf_Append(&p, path);
  FILE* f = fopen(StrBuf_CStr(&p), "wb");
  if (!f) {
    StrBuf_AppendCStr(err, "cannot open file for writing: ");
    StrBuf_Append(err, path);
    return false;
  }

  usize written = fwrite(content.Data, 1, content.Length, f);
  bool ok = written == (usize)content.Length;
  if (fclose(f) != 0) ok = false;
  if (!ok) {
    StrBuf_AppendCStr(err, "failed to write file: ");
    StrBuf_Append(err, path);
    return false;
  }

  StrBuf text = StrBuf_Make(arena, 64 + path.Length);
  StrBuf_AppendCStr(&text, "Successfully wrote ");
  StrBuf_AppendU64(&text, content.Length);
  StrBuf_AppendCStr(&text, " bytes to ");
  StrBuf_Append(&text, path);

  out->Content = Array_Make<Content>(arena, 1);
  Array_Push(&out->Content, Content_Text(StrBuf_View(&text)));
  return true;
}

static bool WriteExec(void* ctx, Arena* arena, StrView id,
                      const ToolArgs* args, volatile i32* abort,
                      const ToolUpdateSink* updates, ToolResult* out,
                      StrBuf* err) {
  (void)ctx;
  (void)id;
  (void)updates;
  StrView path = ToolArgs_GetStr(args, "path");
  if (path.Length == 0) {
    StrBuf_AppendCStr(err, "missing 'path' argument");
    return false;
  }
  StrView content = ToolArgs_GetStr(args, "content");

  StrView key = Fs_RealPath(arena, path);
  FileLockToken tok = FileLock_Acquire(key);
  bool ok = Write_DoLocked(arena, path, content, abort, out, err);
  FileLock_Release(tok);
  return ok;
}

constexpr StrView kWritePromptGuidelines[] = {
    SVL("Use write only for new files or complete rewrites.")};

Tool WriteTool() {
  Tool t = {};
  t.Name = SVL("write");
  t.Label = SVL("Write");
  t.Description = SVL("Write content to a file, creating parent directories as needed.");
  t.PromptSnippet = SVL("Create or overwrite files");
  Tool_SetPromptGuidelines(&t, kWritePromptGuidelines);
  t.ParamsJsonSchema = SVL(
      "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Path to "
      "the file to write (relative or absolute)\"},\"content\":{\"type\":\"string\","
      "\"description\":\"Content to write to the file\"}},\"required\":[\"path\",\"content\"]}");
  t.Execute = WriteExec;
  t.ExecMode = ToolExecMode::Default;
  return t;
}
