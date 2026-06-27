#include "json/json.h"
#include "tools/exec.h"
#include "tools/tools.h"

static const u32 BASH_MAX_OUTPUT = 64 * 1024;

static bool BashExec(void* ctx, Arena* arena, StrView id, StrView argsJson, volatile i32* abort,
                     ToolResult* out, StrBuf* err) {
  (void)ctx;
  (void)id;
  JsonValue* j = Json_Parse(arena, argsJson);
  StrView command = Json_GetStr(j, "command");
  if (command.Length == 0) {
    StrBuf_AppendCStr(err, "missing 'command' argument");
    return false;
  }
  i64 timeout = Json_GetInt(j, "timeout", 120000);
  if (timeout <= 0) timeout = 120000;

  ExecResult r = Exec_Run(arena, command, (u32)timeout, abort);
  if (!r.Spawned) {
    StrBuf_AppendCStr(err, "failed to run command: ");
    StrBuf_Append(err, r.Output);
    return false;
  }

  StrBuf o = StrBuf_Make(arena, r.Output.Length + 64);
  StrView outv = r.Output;
  bool trunc = false;
  if (outv.Length > BASH_MAX_OUTPUT) {
    outv.Length = BASH_MAX_OUTPUT;
    trunc = true;
  }
  StrBuf_Append(&o, outv);
  if (trunc) StrBuf_AppendCStr(&o, "\n... [output truncated at 64KB]");
  if (r.TimedOut) StrBuf_AppendCStr(&o, "\n[command timed out]");
  StrBuf_AppendCStr(&o, "\n[exit code: ");
  StrBuf_AppendI64(&o, r.ExitCode);
  StrBuf_AppendCStr(&o, "]");

  out->Content = Array_Make<Content>(arena, 1);
  Array_Push(&out->Content, Content_Text(StrBuf_View(&o)));
  return true;
}

Tool BashTool() {
  Tool t = {};
  t.Name = SVL("bash");
  t.Label = SVL("Bash");
  t.Description = SVL("Run a shell command and return combined stdout/stderr. Default timeout 120s.");
  t.ParamsJsonSchema = SVL(
      "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"The "
      "shell command to run\"},\"timeout\":{\"type\":\"number\",\"description\":\"Timeout in "
      "milliseconds (default 120000)\"}},\"required\":[\"command\"]}");
  t.Execute = BashExec;
  return t;
}
