#include "json/json.h"
#include "base/thread.h"
#include "crypto/random.h"
#include "tools/exec.h"
#include "tools/fs.h"
#include "tools/tools.h"
#include "tools/truncate.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

static void Shell_AppendHex16(StrBuf* b, u64 v) {
  static const char* hex = "0123456789abcdef";
  for (i32 i = 15; i >= 0; --i) StrBuf_AppendChar(b, hex[(v >> ((u32)i * 4u)) & 0xFu]);
}

static StrView Shell_WriteTempOutput(Arena* arena, StrView output, StrView directory) {
  StrBuf path = StrBuf_Make(arena, 256);
  if (directory.Length) {
    // Created lazily: a session that never overflows leaves no directory.
    if (!Fs_MkdirP(arena, directory)) return StrView{};
    StrBuf_Append(&path, directory);
    char last = path.Data[path.Length - 1];
    if (last != '/' && last != '\\') StrBuf_AppendChar(&path, '/');
    StrBuf_AppendCStr(&path, "shell-");
  } else {
#if defined(_WIN32)
    char temp[MAX_PATH];
    DWORD n = GetTempPathA((DWORD)sizeof(temp), temp);
    if (n == 0 || n >= (DWORD)sizeof(temp)) return StrView{};
    StrBuf_Append(&path, StrView{temp, (u32)n});
    if (path.Length && path.Data[path.Length - 1] != '\\' &&
        path.Data[path.Length - 1] != '/')
      StrBuf_AppendChar(&path, '\\');
#else
    const char* tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0]) tmp = "/tmp";
    StrBuf_AppendCStr(&path, tmp);
    if (path.Length && path.Data[path.Length - 1] != '/')
      StrBuf_AppendChar(&path, '/');
#endif
    StrBuf_AppendCStr(&path, "clanker-shell-");
  }
  u32 prefixLength = path.Length;

  // The OS temp directory is world-writable on POSIX, so a guessable name lets
  // another user pre-create the path (or a symlink to it) and capture or
  // redirect the command output. The name is drawn from the CSPRNG and the
  // file is created exclusively, so an existing path is an error rather than
  // something to overwrite. A session directory is not shared, but it costs
  // nothing to hold it to the same standard.
  for (u32 attempt = 0; attempt < 8; attempt++) {
    path.Length = prefixLength;
    u64 unique = 0;
    RandomBytes((byte*)&unique, sizeof(unique));
    Shell_AppendHex16(&path, unique);
    StrBuf_AppendCStr(&path, ".log");

    const char* target = StrBuf_CStr(&path);
#if defined(_WIN32)
    FILE* f = nullptr;
    if (fopen_s(&f, target, "wbx") != 0) f = nullptr;
#else
    int fd = open(target, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0) continue;
    FILE* f = fdopen(fd, "wb");
    if (!f) {
      close(fd);
      unlink(target);
      continue;
    }
#endif
    if (!f) continue;

    usize written = fwrite(output.Data, 1, output.Length, f);
    bool ok = fclose(f) == 0 && written == output.Length;
    if (!ok) {
      remove(target);
      return StrView{};
    }
    return StrBuf_View(&path);
  }
  return StrView{};
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

static constexpr u32 SHELL_PERMISSION_MAX_COMMAND = 8192;

static bool Shell_IsSimpleCommandName(StrView name) {
  if (name.Length == 0 || name.Data[0] == '-' || name.Data[0] == '.') return false;
  for (u32 i = 0; i < name.Length; i++) {
    char c = name.Data[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')
      continue;
    return false;
  }
  return true;
}

static bool Shell_StageQualifies(StrView stage) {
  StrView tokens[32] = {};
  u32 tokenCount = 0;
  u32 pos = 0;
  while (pos < stage.Length) {
    while (pos < stage.Length && stage.Data[pos] == ' ') pos++;
    if (pos >= stage.Length) break;
    u32 start = pos;
    while (pos < stage.Length && stage.Data[pos] != ' ') pos++;
    if (tokenCount >= (u32)(sizeof(tokens) / sizeof(tokens[0]))) return false;
    tokens[tokenCount++] = StrView{stage.Data + start, pos - start};
  }
  if (tokenCount == 0) return false;

  if (tokenCount == 2 && Shell_IsSimpleCommandName(tokens[0]) &&
      (StrView_EqCStr(tokens[1], "--help") ||
       StrView_EqCStr(tokens[1], "--version")))
    return true;

  if (StrView_EqCStr(tokens[0], "git")) {
    if (tokenCount < 2) return false;
    for (u32 i = 2; i < tokenCount; i++) {
      StrView token = tokens[i];
      bool output =
          StrView_EqCStr(token, "--output") ||
          (token.Length > sizeof("--output=") - 1u &&
           memcmp(token.Data, "--output=", sizeof("--output=") - 1u) == 0);
      if (output || StrView_EqCStr(token, "--ext-diff") ||
          StrView_EqCStr(token, "--textconv"))
        return false;
    }
    if (StrView_EqCStr(tokens[1], "status") ||
        StrView_EqCStr(tokens[1], "diff") ||
        StrView_EqCStr(tokens[1], "log") ||
        StrView_EqCStr(tokens[1], "show") ||
        StrView_EqCStr(tokens[1], "rev-parse") ||
        StrView_EqCStr(tokens[1], "ls-files"))
      return true;
    return StrView_EqCStr(tokens[1], "branch") && tokenCount == 3 &&
           StrView_EqCStr(tokens[2], "--show-current");
  }

  return false;
}

static bool Shell_AppendPermissionChar(StrBuf* out, char c, bool* tooLong) {
  if (out->Length + 1u >= SHELL_PERMISSION_MAX_COMMAND) {
    *tooLong = true;
    return false;
  }
  StrBuf_AppendChar(out, c);
  return true;
}

static void Shell_AppendPermissionText(StrBuf* out, const char* text,
                                       bool* tooLong) {
  for (const char* p = text; *p; p++)
    Shell_AppendPermissionChar(out, *p, tooLong);
}

static StrView Shell_CanonicalPermissionCommand(Arena* arena, StrView command,
                                                bool* autoAllow,
                                                bool* stableCanonical) {
  StrBuf canonical = StrBuf_Make(
      arena, ClankerMin(command.Length + 1u, SHELL_PERMISSION_MAX_COMMAND));
  bool safe = true;
  bool tooLong = false;
  bool pendingSpace = false;
  for (u32 i = 0; i < command.Length; i++) {
    char c = command.Data[i];
    if (Ascii_IsSpace(c)) {
      if (c == '\r' || c == '\n') safe = false;
      pendingSpace = canonical.Length != 0;
      continue;
    }

    if (c == '&' && i + 1u < command.Length && command.Data[i + 1u] == '&') {
      if (canonical.Length && canonical.Data[canonical.Length - 1u] != ' ')
        Shell_AppendPermissionChar(&canonical, ' ', &tooLong);
      Shell_AppendPermissionText(&canonical, "&& ", &tooLong);
      pendingSpace = false;
      i++;
      continue;
    }

    if (pendingSpace && canonical.Length &&
        canonical.Data[canonical.Length - 1u] != ' ')
      Shell_AppendPermissionChar(&canonical, ' ', &tooLong);
    pendingSpace = false;

    switch (c) {
      case '|':
      case '<':
      case '>':
      case ';':
      case '(':
      case ')':
      case '$':
      case '`':
      case '\'':
      case '"':
      case '\\':
      case '*':
      case '?':
      case '~':
      case '#':
      case '%':
      case '!':
      case '^':
      case '{':
      case '}':
      case '[':
      case ']':
        safe = false;
        break;
      case '&':
        safe = false;
        break;
      default:
        break;
    }
    Shell_AppendPermissionChar(&canonical, c, &tooLong);
  }

  while (canonical.Length && canonical.Data[canonical.Length - 1u] == ' ') {
    canonical.Length--;
    canonical.Data[canonical.Length] = '\0';
  }

  bool qualifies = safe && !tooLong;
  u32 start = 0;
  for (u32 i = 0; qualifies && i <= canonical.Length; i++) {
    bool split = i == canonical.Length ||
                 (i + 1u < canonical.Length && canonical.Data[i] == '&' &&
                  canonical.Data[i + 1u] == '&');
    if (!split) continue;
    u32 end = i;
    while (start < end && canonical.Data[start] == ' ') start++;
    while (end > start && canonical.Data[end - 1u] == ' ') end--;
    if (!Shell_StageQualifies(StrView{canonical.Data + start, end - start}))
      qualifies = false;
    start = i + 2u;
    i++;
  }
  *autoAllow = qualifies;
  *stableCanonical = safe && !tooLong;
  return StrBuf_View(&canonical);
}

static bool ShellDescribePermission(void* ctx, Arena* arena,
                                    const ToolArgs* args,
                                    StrView workingDirectory,
                                    ToolPermissionInfo* out, StrBuf* err) {
  (void)ctx;
  (void)workingDirectory;
  StrView command = ToolArgs_GetStr(args, "command");
  if (command.Length == 0) {
    StrBuf_AppendCStr(err, "missing 'command' argument");
    return false;
  }

  bool autoAllow = false;
  bool stableCanonical = false;
  StrView canonical =
      Shell_CanonicalPermissionCommand(arena, command, &autoAllow,
                                       &stableCanonical);
  StrView identity = stableCanonical ? canonical : command;
  StrView shown = identity.Length <= 2048 ? identity
                                          : StrView{identity.Data, 2048};
  StrBuf action = StrBuf_Make(arena, shown.Length + sizeof("Run "));
  StrBuf_AppendCStr(&action, "Run ");
  StrBuf_Append(&action, shown);
  out->Action = StrBuf_View(&action);
  if (identity.Length + sizeof("shell\n") - 1u <=
      SHELL_PERMISSION_MAX_COMMAND) {
    StrBuf key = StrBuf_Make(arena, identity.Length + 16u);
    StrBuf_AppendCStr(&key, "shell\n");
    StrBuf_Append(&key, identity);
    out->GrantKey = StrBuf_View(&key);
  }
  out->AutoAllow = autoAllow;
  return true;
}

static bool ShellExec(void* ctx, Arena* arena, StrView id,
                      const ToolArgs* args, volatile i32* abort,
                      const ToolUpdateSink* updates, ToolResult* out,
                      StrBuf* err) {
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
    const ShellToolContext* shell = (const ShellToolContext*)ctx;
    StrView path = Shell_WriteTempOutput(arena, r.Output,
                                         shell ? shell->Directory : StrView{});
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

Tool ShellTool(ShellToolContext* context) {
  Tool t = {};
  t.Name = SVL("shell");
  t.Label = SVL("Shell");
  t.Description = SVL("Run a shell command and return combined stdout/stderr.");
  t.PromptSnippet = SVL("Execute shell commands (ls, grep, find, etc.)");
  t.ParamsJsonSchema = SVL(
      "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"Shell "
      "command to execute\"},\"timeout\":{\"type\":\"number\",\"description\":\"Timeout in seconds "
      "(optional, no default timeout)\"}},\"required\":[\"command\"]}");
  t.Ctx = context;
  t.PermissionEffect = ToolPermissionEffect::Sensitive;
  t.DescribePermission = ShellDescribePermission;
  t.Execute = ShellExec;
  return t;
}
