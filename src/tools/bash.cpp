#include "json/json.h"
#include "tools/exec.h"
#include "tools/tools.h"
#include "tools/truncate.h"

#include <stdio.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#endif

static void Bash_AppendHex16(StrBuf* b, u64 v) {
  static const char* hex = "0123456789abcdef";
  for (i32 i = 15; i >= 0; --i) StrBuf_AppendChar(b, hex[(v >> ((u32)i * 4u)) & 0xFu]);
}

static StrView Bash_WriteTempOutput(Arena* arena, StrView output) {
  static u32 counter;
  counter++;

  StrBuf path = StrBuf_Make(arena, 256);
#if defined(_WIN32)
  char temp[MAX_PATH];
  DWORD n = GetTempPathA((DWORD)sizeof(temp), temp);
  if (n == 0 || n >= (DWORD)sizeof(temp)) return StrView{};
  StrBuf_Append(&path, StrView{temp, (u32)n});
  if (path.Length && path.Data[path.Length - 1] != '\\' && path.Data[path.Length - 1] != '/')
    StrBuf_AppendChar(&path, '\\');
  u64 unique = ((u64)GetTickCount() << 32) ^ (u64)GetCurrentProcessId() ^ (u64)counter;
#else
  const char* tmp = getenv("TMPDIR");
  if (!tmp || !tmp[0]) tmp = "/tmp";
  StrBuf_AppendCStr(&path, tmp);
  if (path.Length && path.Data[path.Length - 1] != '/') StrBuf_AppendChar(&path, '/');
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  u64 unique = ((u64)(u32)getpid() << 32) ^ (u64)(u32)ts.tv_nsec ^ (u64)counter;
#endif
  StrBuf_AppendCStr(&path, "pi-bash-");
  Bash_AppendHex16(&path, unique);
  StrBuf_AppendCStr(&path, ".log");

  FILE* f = fopen(StrBuf_CStr(&path), "wb");
  if (!f) return StrView{};
  usize written = fwrite(output.Data, 1, output.Length, f);
  bool closed = fclose(f) == 0;
  bool ok = written == output.Length && closed;
  if (!ok) return StrView{};
  return StrBuf_View(&path);
}

static void Bash_AppendTruncNotice(StrBuf* b, TruncResult tr, StrView path) {
  u32 a = tr.TotalLines > tr.OutputLines ? tr.TotalLines - tr.OutputLines + 1u : 1u;
  u32 c = tr.TotalLines;
  StrBuf_AppendCStr(b, "\n\n[Showing lines ");
  StrBuf_AppendU64(b, a);
  StrBuf_AppendChar(b, '-');
  StrBuf_AppendU64(b, c);
  StrBuf_AppendCStr(b, " of ");
  StrBuf_AppendU64(b, tr.TotalLines);
  if (tr.ByBytes) StrBuf_AppendCStr(b, " (50.0KB limit)");
  StrBuf_AppendCStr(b, ". Full output: ");
  StrBuf_Append(b, path);
  StrBuf_AppendChar(b, ']');
}

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
  i64 sec = Json_GetInt(j, "timeout", 0);
  u32 ms = sec > 0 ? (u32)(sec * 1000) : 0;

  ExecResult r = Exec_Run(arena, command, ms, abort);
  if (!r.Spawned) {
    StrBuf_AppendCStr(err, "failed to run command: ");
    StrBuf_Append(err, r.Output);
    return false;
  }

  TruncResult tr = Trunc_Tail(arena, r.Output, TRUNC_MAX_LINES, TRUNC_MAX_BYTES);
  StrBuf text = StrBuf_Make(arena, r.Output.Length ? tr.Content.Length + 256 : 64);
  if (r.Output.Length == 0) {
    StrBuf_AppendCStr(&text, "(no output)");
  } else {
    StrBuf_Append(&text, tr.Content);
  }
  if (tr.Truncated) {
    StrView path = Bash_WriteTempOutput(arena, r.Output);
    if (path.Length) Bash_AppendTruncNotice(&text, tr, path);
  }

  if (r.TimedOut) {
    StrBuf_AppendCStr(&text, "\n\nCommand timed out after ");
    StrBuf_AppendI64(&text, sec);
    StrBuf_AppendCStr(&text, " seconds");
    StrBuf_Append(err, StrBuf_View(&text));
    return false;
  }
  if (r.ExitCode != 0) {
    StrBuf_AppendCStr(&text, "\n\nCommand exited with code ");
    StrBuf_AppendI64(&text, r.ExitCode);
    StrBuf_Append(err, StrBuf_View(&text));
    return false;
  }

  out->Content = Array_Make<Content>(arena, 1);
  Array_Push(&out->Content, Content_Text(StrBuf_View(&text)));
  return true;
}

Tool BashTool() {
  Tool t = {};
  t.Name = SVL("bash");
  t.Label = SVL("Bash");
  t.Description = SVL("Run a bash command and return combined stdout/stderr.");
  t.ParamsJsonSchema = SVL(
      "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"Bash "
      "command to execute\"},\"timeout\":{\"type\":\"number\",\"description\":\"Timeout in seconds "
      "(optional, no default timeout)\"}},\"required\":[\"command\"]}");
  t.ReplayPolicy = ToolReplayPolicy::ManualReconcile;
  t.Execute = BashExec;
  return t;
}
