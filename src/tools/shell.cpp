#include "json/json.h"
#include "base/file_io.h"
#include "base/thread.h"
#include "crypto/random.h"
#include "tools/exec.h"
#include "tools/fs.h"
#include "tools/shell_capture.h"
#include "tools/tools.h"
#include "tools/truncate.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include "base/win32_utf8.h"
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

static void* Shell_OpenOutput(void*, Arena* arena, StrView directory, StrView* outPath) {
  StrBuf path = StrBuf_Make(arena, 256);
  if (directory.Length) {
    // Created lazily: a session that never overflows leaves no directory.
    if (!Fs_MkdirP(arena, directory)) return nullptr;
    StrBuf_Append(&path, directory);
    char last = path.Data[path.Length - 1];
    if (last != '/' && last != '\\') StrBuf_AppendChar(&path, '/');
    StrBuf_AppendCStr(&path, "shell-");
  } else {
#if defined(_WIN32)
    wchar_t temp[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, temp);
    StrView tempDir = {};
    if (n == 0 || n > MAX_PATH || !Win32_Utf8(arena, temp, n, &tempDir))
      return nullptr;
    StrBuf_Append(&path, tempDir);
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

#if defined(_WIN32)
    FILE* f = nullptr;
    if (File_Open(arena, StrBuf_View(&path), "wbx", &f) != FileIoResult::Success)
      f = nullptr;
#else
    const char* target = StrBuf_CStr(&path);
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

    *outPath = StrBuf_View(&path);
    return f;
  }
  return nullptr;
}

static bool Shell_WriteOutput(void*, void* file, StrView bytes) {
  return fwrite(bytes.Data, 1, bytes.Length, (FILE*)file) == bytes.Length;
}

static bool Shell_CloseOutput(void*, void* file) {
  return fclose((FILE*)file) == 0;
}

// The extra byte preserves whether the byte-limited tail starts immediately
// after a newline. Trunc_Tail also uses it when snapping a UTF-8 boundary.
static constexpr u32 SHELL_TAIL_CAPACITY = TRUNC_MAX_BYTES + 1;

void ShellCapture_Init(ShellCapture* capture, Arena* arena, StrView directory,
                       const ShellCaptureIo* io) {
  *capture = {};
  capture->Mem = arena;
  capture->Directory = directory;
  capture->Tail = ArenaPushArray(arena, char, SHELL_TAIL_CAPACITY);
  capture->Io = io ? *io : ShellCaptureIo{nullptr, Shell_OpenOutput,
                                         Shell_WriteOutput, Shell_CloseOutput};
  FFI(!capture->Io.Open || !capture->Io.Write || !capture->Io.Close);
}

u64 ShellCapture_LineCount(const ShellCapture* capture) {
  return capture->Newlines +
         (capture->TotalBytes && capture->LastByte != '\n' ? 1u : 0u);
}

static bool ShellCapture_Write(ShellCapture* capture, StrView bytes) {
  if (!bytes.Length) return true;
  if (capture->Io.Write(capture->Io.User, capture->File, bytes)) return true;
  capture->Error = SVL("failed to write shell output log");
  return false;
}

bool ShellCapture_Feed(ShellCapture* capture, StrView chunk) {
  FFI(capture->Finished);
  if (capture->Error.Length) return false;
  if (!chunk.Length) return true;
  if (capture->TotalBytes > ~(u64)0 - chunk.Length) {
    capture->Error = SVL("shell output exceeded the supported byte count");
    return false;
  }
  capture->TotalBytes += chunk.Length;
  for (u32 i = 0; i < chunk.Length; i++)
    if (chunk.Data[i] == '\n') capture->Newlines++;
  capture->LastByte = chunk.Data[chunk.Length - 1];

  if (!capture->File &&
      (capture->TotalBytes > TRUNC_MAX_BYTES ||
       ShellCapture_LineCount(capture) > TRUNC_MAX_LINES)) {
    capture->File = capture->Io.Open(capture->Io.User, capture->Mem,
                                      capture->Directory, &capture->Path);
    if (!capture->File) {
      capture->Error = SVL("failed to create shell output log");
    } else {
      // Before the first overflow the ring still holds every earlier byte.
      u32 first = ClankerMin(capture->Count, SHELL_TAIL_CAPACITY - capture->Head);
      if (ShellCapture_Write(capture, StrView{capture->Tail + capture->Head, first}))
        ShellCapture_Write(capture, StrView{capture->Tail, capture->Count - first});
    }
  }
  if (capture->File && !capture->Error.Length)
    ShellCapture_Write(capture, chunk);

  if (chunk.Length >= SHELL_TAIL_CAPACITY) {
    memcpy(capture->Tail, chunk.Data + chunk.Length - SHELL_TAIL_CAPACITY,
           SHELL_TAIL_CAPACITY);
    capture->Head = 0;
    capture->Count = SHELL_TAIL_CAPACITY;
  } else {
    u32 dropped = capture->Count + chunk.Length > SHELL_TAIL_CAPACITY
                      ? capture->Count + chunk.Length - SHELL_TAIL_CAPACITY : 0;
    capture->Head = (capture->Head + dropped) % SHELL_TAIL_CAPACITY;
    capture->Count -= dropped;
    u32 end = (capture->Head + capture->Count) % SHELL_TAIL_CAPACITY;
    u32 first = ClankerMin(chunk.Length, SHELL_TAIL_CAPACITY - end);
    memcpy(capture->Tail + end, chunk.Data, first);
    memcpy(capture->Tail, chunk.Data + first, chunk.Length - first);
    capture->Count += chunk.Length;
  }
  return !capture->Error.Length;
}

bool ShellCapture_Finish(ShellCapture* capture) {
  if (!capture->Finished && capture->File) {
    bool closed = capture->Io.Close(capture->Io.User, capture->File);
    capture->File = nullptr;
    if (!closed && !capture->Error.Length)
      capture->Error = SVL("failed to close shell output log");
  }
  capture->Finished = true;
  return !capture->Error.Length;
}

StrView ShellCapture_Tail(Arena* scratch, const ShellCapture* capture, bool* byBytes) {
  char* text = ArenaPushArray(scratch, char, capture->Count + 1);
  u32 first = ClankerMin(capture->Count, SHELL_TAIL_CAPACITY - capture->Head);
  memcpy(text, capture->Tail + capture->Head, first);
  memcpy(text + first, capture->Tail, capture->Count - first);
  text[capture->Count] = '\0';
  TruncResult tail = Trunc_Tail(scratch, StrView{text, capture->Count},
                               TRUNC_MAX_LINES, TRUNC_MAX_BYTES);
  if (byBytes) *byBytes = tail.ByBytes;
  return tail.Content;
}

static void Shell_AppendTruncNotice(StrBuf* b, const ShellCapture* capture,
                                    StrView tail, bool byBytes) {
  u64 c = ShellCapture_LineCount(capture);
  u32 outputLines = Trunc_CountLines(tail);
  u64 a = c > outputLines ? c - outputLines + 1u : 1u;
  StrBuf_AppendCStr(b, "\n\n[Showing lines ");
  StrBuf_AppendU64(b, a);
  StrBuf_AppendChar(b, '-');
  StrBuf_AppendU64(b, c);
  StrBuf_AppendCStr(b, " of ");
  StrBuf_AppendU64(b, c);
  if (byBytes) StrBuf_AppendCStr(b, " (50.0KB limit)");
  StrBuf_AppendCStr(b, ". Full output: ");
  StrBuf_Append(b, capture->Path);
  StrBuf_AppendChar(b, ']');
}

struct ShellStreamCapture {
  Arena Snapshot;
  ShellCapture Output;
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
  StrView tail = ShellCapture_Tail(&capture->Snapshot, &capture->Output);
  ToolResult partial = {};
  partial.Content = Array_Make<Content>(&capture->Snapshot, 1);
  Array_Push(&partial.Content, Content_Text(tail));
  ToolUpdateSink_Emit(capture->Updates, &partial);
  capture->LastUpdateMs = now;
  capture->Dirty = false;
}

static bool Shell_OnData(void* user, StrView chunk) {
  ShellStreamCapture* capture = (ShellStreamCapture*)user;
  bool ok = ShellCapture_Feed(&capture->Output, chunk);
  capture->Dirty = true;
  Shell_EmitPartial(capture, false);
  return ok;
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
                                    const ToolPermissionScope* scope,
                                    ToolPermissionInfo* out, StrBuf* err) {
  (void)ctx;
  (void)scope;
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
  StrBuf action = StrBuf_Make(arena, identity.Length + sizeof("Run "));
  StrBuf_AppendCStr(&action, "Run ");
  StrBuf_Append(&action, identity);
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
  StrView command = ToolArgs_GetStr(args, "command");
  if (command.Length == 0) {
    StrBuf_AppendCStr(err, "missing 'command' argument");
    return false;
  }
  i64 sec = ToolArgs_GetInt(args, "timeout", 0);
  if (sec > 0xFFFFFFFFu / 1000u) {
    StrBuf_AppendCStr(err, "timeout exceeds the supported duration");
    return false;
  }
  u32 ms = sec > 0 ? (u32)(sec * 1000) : 0;

  ToolResult initial = {};
  initial.Content = Array_Make<Content>(arena, 1);
  Array_Push(&initial.Content, Content_Text(SVL("")));
  ToolUpdateSink_Emit(updates, &initial);

  Arena work = ArenaCreate();
  ShellStreamCapture capture = {};
  capture.Snapshot = ArenaCreate((usize)256 << 10);
  const ShellToolContext* shell = (const ShellToolContext*)ctx;
  ShellCapture_Init(&capture.Output, &work, shell ? shell->Directory : StrView{},
                     shell ? shell->OutputIo : nullptr);
  capture.Updates = updates;
  ExecResult r =
      Exec_RunStreaming(&work, command, ms, abort, Shell_OnData, &capture);
  ShellCapture_Finish(&capture.Output);
  Shell_EmitPartial(&capture, true);
  if (!r.Spawned) {
    StrBuf_AppendCStr(err, "failed to run command: ");
    StrBuf_Append(err, r.Output);
    ArenaRelease(&capture.Snapshot);
    ArenaRelease(&work);
    return false;
  }

  ArenaReset(&capture.Snapshot);
  bool byBytes = false;
  StrView tail = ShellCapture_Tail(&capture.Snapshot, &capture.Output, &byBytes);
  StrBuf text = StrBuf_Make(arena, tail.Length + capture.Output.Path.Length + 256);
  if (capture.Output.TotalBytes == 0) {
    StrBuf_AppendCStr(&text, "(no output)");
  } else {
    StrBuf_Append(&text, tail);
  }
  bool failed = false;
  StrView failure = capture.Output.Error.Length ? capture.Output.Error : r.Output;
  if (failure.Length) {
    StrBuf_AppendCStr(&text, "\n\n");
    StrBuf_Append(&text, failure);
    if (capture.Output.Path.Length) {
      StrBuf_AppendCStr(&text, ". Incomplete output: ");
      StrBuf_Append(&text, capture.Output.Path);
    }
    failed = true;
  } else if (capture.Output.TotalBytes > tail.Length) {
    Shell_AppendTruncNotice(&text, &capture.Output, tail, byBytes);
  }
  if (abort && Atomic_LoadI32(abort)) {
    StrBuf_AppendCStr(&text, "\n\nCommand cancelled");
    failed = true;
  } else if (r.TimedOut) {
    StrBuf_AppendCStr(&text, "\n\nCommand timed out after ");
    StrBuf_AppendI64(&text, sec);
    StrBuf_AppendCStr(&text, " seconds");
    failed = true;
  } else if (r.ExitCode != 0 && !failed) {
    StrBuf_AppendCStr(&text, "\n\nCommand exited with code ");
    StrBuf_AppendI64(&text, r.ExitCode);
    failed = true;
  }
  ArenaRelease(&capture.Snapshot);
  ArenaRelease(&work);
  if (failed) {
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
