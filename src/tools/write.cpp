#include "base/file_io.h"
#include "base/thread.h"
#include "json/json.h"
#include "tools/filelock.h"
#include "tools/fs.h"
#include "tools/tools.h"

static bool Write_DoLocked(Arena* arena, StrView path, StrView content, volatile i32* abort,
                           ToolResult* out, StrBuf* err) {
  if (abort && Atomic_LoadI32(abort)) {
    StrBuf_AppendCStr(err, "aborted");
    return false;
  }

  StrView dir = Fs_Parent(path);
  if (dir.Length != 0 && !Fs_MkdirP(arena, dir)) {
    StrBuf_AppendCStr(err, "failed to create parent directory for: ");
    StrBuf_Append(err, path);
    return false;
  }

  FileIoResult written = File_WriteAll(arena, path, content);
  if (written != FileIoResult::Success) {
    StrBuf_AppendCStr(err, written == FileIoResult::IoError
                             ? "failed to write file: " : "cannot open file for writing: ");
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

static bool WriteDescribePermission(void* ctx, Arena* arena,
                                    const ToolArgs* args,
                                    StrView workingDirectory,
                                    ToolPermissionInfo* out, StrBuf* err) {
  (void)ctx;
  return Fs_DescribeMutationPermission(
      arena, args, workingDirectory, SVL("Write file "), SVL("write\n"), out,
      err);
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
  t.PermissionEffect = ToolPermissionEffect::Sensitive;
  t.DescribePermission = WriteDescribePermission;
  return t;
}
