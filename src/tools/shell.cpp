#include "json/json.h"
#include "base/thread.h"
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

static void Shell_AppendHex16(StrBuf* b, u64 v) {
  static const char* hex = "0123456789abcdef";
  for (i32 i = 15; i >= 0; --i) StrBuf_AppendChar(b, hex[(v >> ((u32)i * 4u)) & 0xFu]);
}

static StrView Shell_WriteTempOutput(Arena* arena, StrView output) {
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
  StrBuf_AppendCStr(&path, "clank-shell-");
  Shell_AppendHex16(&path, unique);
  StrBuf_AppendCStr(&path, ".log");

  FILE* f = fopen(StrBuf_CStr(&path), "wb");
  if (!f) return StrView{};
  usize written = fwrite(output.Data, 1, output.Length, f);
  bool closed = fclose(f) == 0;
  bool ok = written == output.Length && closed;
  if (!ok) return StrView{};
  return StrBuf_View(&path);
}

static void Shell_AppendTruncNotice(StrBuf* b, TruncResult tr, StrView path) {
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

struct ShellStreamCapture {
  Arena Snapshot;
  StrBuf Output;
  const ToolUpdateSink* Updates;
  u64 LastUpdateMs;
  bool Dirty;
};

static void Shell_EmitPartial(ShellStreamCapture* capture, bool force) {
  if (!capture->Updates || !capture->Updates->OnUpdate || !capture->Dirty)
    return;
  u64 now = MonotonicMillis();
  if (!force && capture->LastUpdateMs &&
      now - capture->LastUpdateMs < 100)
    return;

  ArenaReset(&capture->Snapshot);
  TruncResult truncated =
      Trunc_Tail(&capture->Snapshot, StrBuf_View(&capture->Output),
                 TRUNC_MAX_LINES, TRUNC_MAX_BYTES);
  ToolResult partial = {};
  partial.Content = Array_Make<Content>(&capture->Snapshot, 1);
  Array_Push(&partial.Content, Content_Text(truncated.Content));
  ToolUpdateSink_Emit(capture->Updates, &partial);
  capture->LastUpdateMs = now;
  capture->Dirty = false;
}

static bool Shell_OnData(void* user, StrView chunk) {
  ShellStreamCapture* capture = (ShellStreamCapture*)user;
  (void)chunk;
  capture->Dirty = true;
  Shell_EmitPartial(capture, false);
  return true;
}

static bool ShellExec(void* ctx, Arena* arena, StrView id,
                      const ToolArgs* args, volatile i32* abort,
                      const ToolUpdateSink* updates, ToolResult* out,
                      StrBuf* err) {
  (void)ctx;
  (void)id;
  (void)updates;
  StrView command = ToolArgs_GetStr(args, "command");
  if (command.Length == 0) {
    StrBuf_AppendCStr(err, "missing 'command' argument");
    return false;
  }
  i64 sec = ToolArgs_GetInt(args, "timeout", 0);
  u32 ms = sec > 0 ? (u32)(sec * 1000) : 0;

  ToolResult initial = {};
  initial.Content = Array_Make<Content>(arena, 1);
  Array_Push(&initial.Content, Content_Text(SVL("")));
  ToolUpdateSink_Emit(updates, &initial);

  ShellStreamCapture capture = {};
  capture.Snapshot = ArenaCreate((usize)16 << 20);
  capture.Output = StrBuf_Make(arena, 4096);
  capture.Updates = updates;
  ExecResult r =
      Exec_RunStreaming(arena, command, ms, abort, Shell_OnData, &capture,
                        &capture.Output);
  Shell_EmitPartial(&capture, true);
  ArenaRelease(&capture.Snapshot);
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
    StrView path = Shell_WriteTempOutput(arena, r.Output);
    if (path.Length) Shell_AppendTruncNotice(&text, tr, path);
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

Tool ShellTool() {
  Tool t = {};
  t.Name = SVL("shell");
  t.Label = SVL("Shell");
  t.Description = SVL("Run a shell command and return combined stdout/stderr.");
  t.PromptSnippet = SVL("Execute shell commands (ls, grep, find, etc.)");
  t.ParamsJsonSchema = SVL(
      "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"Shell "
      "command to execute\"},\"timeout\":{\"type\":\"number\",\"description\":\"Timeout in seconds "
      "(optional, no default timeout)\"}},\"required\":[\"command\"]}");
  t.Execute = ShellExec;
  return t;
}
