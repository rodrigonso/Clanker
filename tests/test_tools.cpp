#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/arena.h"
#include "base/str.h"
#include "base/sysinfo.h"
#include "base/thread.h"
#include "check.h"
#include "tools/exec.h"
#include "tools/fs.h"
#include "tools/imagemime.h"
#include "tools/tools.h"
#include "tools/tools_manager.h"
#include "tools/truncate.h"
#include "tests.h"

#if defined(_WIN32)
#include <direct.h>
#define CLANKER_RMDIR _rmdir
#else
#include <unistd.h>
#define CLANKER_RMDIR rmdir
#endif

// ---- helpers -------------------------------------------------------------

static void MakeFile(const char* path, const char* content) {
  FILE* f = fopen(path, "wb");
  if (f) {
    fwrite(content, 1, strlen(content), f);
    fclose(f);
  }
}

static StrView Slurp(Arena* a, const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return StrView{};
  StrBuf b = StrBuf_Make(a, 256);
  char buf[1024];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) StrBuf_Append(&b, StrView{buf, (u32)n});
  fclose(f);
  return StrBuf_View(&b);
}

static bool RunTool(const Tool& t, Arena* a, StrView argsJson, ToolResult* out,
                    StrBuf* err, const ToolUpdateSink* updates = nullptr) {
  JsonSchema schema = {};
  if (!JsonSchema_Compile(a, t.ParamsJsonSchema, &schema, err)) return false;
  JsonValue* root = Json_Parse(a, argsJson);
  if (!root) {
    StrBuf_AppendCStr(err, "invalid JSON");
    return false;
  }
  if (t.PrepareArguments) {
    JsonValue* prepared = root;
    if (!t.PrepareArguments(t.Ctx, a, root, &prepared, err)) return false;
    root = prepared;
  }
  if (!JsonSchema_Validate(a, &schema, root, err)) return false;
  StrBuf canonical = StrBuf_Make(a, argsJson.Length + 32);
  Json_WriteValue(&canonical, root);
  ToolArgs args = {root, StrBuf_View(&canonical)};
  return t.Execute(t.Ctx, a, SV("id"), &args, nullptr, updates, out, err);
}

static void SetTestEnv(const char* name, const char* value) {
#if defined(_WIN32)
  _putenv_s(name, value ? value : "");
#else
  if (value)
    setenv(name, value, 1);
  else
    unsetenv(name);
#endif
}

static bool CaptureChunk(void* user, StrView chunk) {
  StrBuf_Append((StrBuf*)user, chunk);
  return true;
}

static void TestBundledRipgrep(Arena* a) {
  StrView cwd = Sys_CurrentDir(a);
  StrBuf rootName = StrBuf_Make(a, 64);
  StrBuf_AppendCStr(&rootName, "out/clanker-rg-discovery-");
  StrBuf_AppendU64(&rootName, MonotonicMillis());
  StrView root = Fs_Join(a, cwd, StrBuf_View(&rootName));
  StrView bin = Fs_Join(a, root, SVL("bin"));
  StrView elsewhere = Fs_Join(a, root, SVL("elsewhere"));
  CHECK(Fs_MkdirP(a, bin));
  CHECK(Fs_MkdirP(a, elsewhere));
#if defined(_WIN32)
  StrView rgName = SVL("rg.exe");
#else
  StrView rgName = SVL("rg");
#endif
  StrView executable = Fs_Join(a, bin, SVL("clanker"));
  StrView bundled = Fs_Join(a, bin, rgName);
  StrView decoy = Fs_Join(a, elsewhere, rgName);
  MakeFile(StrView_DupCStr(a, decoy), "not the bundled tool");
  CHECK(Sys_SetCurrentDir(elsewhere));
  CHECK(ToolManager_FindBundledRipgrep(a, executable).Length == 0);
  CHECK(ToolManager_FindBundledRipgrep(a, SVL("clanker")).Length == 0);
  CHECK(ToolManager_FindBundledRipgrep(a, StrView{}).Length == 0);
  CHECK(ToolManager_FindBundledRipgrep(a, SVL("/")).Length == 0);
  CHECK(ToolManager_FindBundledRipgrep(a, SVL("C:\\")).Length == 0);
  MakeFile(StrView_DupCStr(a, bundled), "bundled tool");
  CHECK(Fs_PathEquals(ToolManager_FindBundledRipgrep(a, executable), bundled));

  // An explicit missing override must fail, not silently use another rg.
  const char* previousRaw = getenv("CLANKER_RG_PATH");
  StrView previous = previousRaw ? StrView_Dup(a, SV(previousRaw)) : StrView{};
  SetTestEnv("CLANKER_RG_PATH", StrView_DupCStr(a, decoy));
  StrBuf err = StrBuf_Make(a, 128);
  CHECK(Fs_PathEquals(ToolManager_EnsureRipgrep(a, nullptr, &err), decoy));
  CHECK(err.Length == 0);
  StrView missing = Fs_Join(a, root, SVL("missing-rg"));
  SetTestEnv("CLANKER_RG_PATH", StrView_DupCStr(a, missing));
  CHECK(ToolManager_EnsureRipgrep(a, nullptr, &err).Length == 0);
  CHECK(StrView_Contains(StrBuf_View(&err), "CLANKER_RG_PATH does not exist"));
  SetTestEnv("CLANKER_RG_PATH", previousRaw ? StrView_DupCStr(a, previous) : nullptr);

  CHECK(Sys_SetCurrentDir(cwd));
  remove(StrView_DupCStr(a, bundled));
  CHECK(Fs_MkdirP(a, bundled));
  CHECK(ToolManager_FindBundledRipgrep(a, executable).Length == 0);
  CLANKER_RMDIR(StrView_DupCStr(a, bundled));
  remove(StrView_DupCStr(a, decoy));
  CLANKER_RMDIR(StrView_DupCStr(a, bin));
  CLANKER_RMDIR(StrView_DupCStr(a, elsewhere));
  CLANKER_RMDIR(StrView_DupCStr(a, root));
}

static StrView Args1(Arena* a, const char* k, const char* v) {
  StrBuf b = StrBuf_Make(a, 64);
  StrBuf_AppendCStr(&b, "{\"");
  StrBuf_AppendCStr(&b, k);
  StrBuf_AppendCStr(&b, "\":\"");
  StrBuf_AppendCStr(&b, v);
  StrBuf_AppendCStr(&b, "\"}");
  return StrBuf_View(&b);
}

static bool DescribeToolPermission(Arena* a, const Tool& tool,
                                   StrView argsJson, StrView workingDirectory,
                                   ToolPermissionInfo* out, StrBuf* err) {
  JsonValue* root = Json_ParseCopy(a, argsJson);
  if (!root) {
    StrBuf_AppendCStr(err, "invalid JSON");
    return false;
  }
  StrBuf canonical = StrBuf_Make(a, argsJson.Length + 32);
  Json_WriteValue(&canonical, root);
  ToolArgs args = {root, StrBuf_View(&canonical)};
  if (!tool.DescribePermission) return false;
  return tool.DescribePermission(tool.Ctx, a, &args, workingDirectory, out, err);
}

static void TestToolPermissions(Arena* a) {
  Tool read = ReadTool();
  Tool ls = LsTool();
  Tool find = FindTool();
  Tool grep = GrepTool();
  Tool ask = AskUserTool();
  CHECK_ALL(read.PermissionEffect == ToolPermissionEffect::Inspection,
            ls.PermissionEffect == ToolPermissionEffect::Inspection,
            find.PermissionEffect == ToolPermissionEffect::Inspection,
            grep.PermissionEffect == ToolPermissionEffect::Inspection,
            ask.PermissionEffect == ToolPermissionEffect::UserInteraction);

  const char* root = "clanker_tool_permission_root";
  Fs_MkdirP(a, SV("clanker_tool_permission_root/sub"));
  StrView workingDirectory = Fs_RealPath(a, SV(root));

  Tool write = WriteTool();
  Tool edit = EditTool();
  CHECK_ALL(write.PermissionEffect == ToolPermissionEffect::Sensitive,
            edit.PermissionEffect == ToolPermissionEffect::Sensitive,
            write.DescribePermission != nullptr,
            edit.DescribePermission != nullptr);

  ToolPermissionInfo inside = {};
  StrBuf insideErr = StrBuf_Make(a, 64);
  CHECK(DescribeToolPermission(
      a, write, SV("{\"path\":\"sub/../new.txt\",\"content\":\"x\"}"),
      workingDirectory, &inside, &insideErr));
  CHECK_ALL(inside.AutoAllow, inside.Action.Length > 0,
            inside.Action.Length <= 2048, inside.GrantKey.Length > 0,
            inside.GrantKey.Length <= 8192);

  ToolPermissionInfo sameTarget = {};
  StrBuf sameTargetErr = StrBuf_Make(a, 64);
  CHECK(DescribeToolPermission(
      a, write, SV("{\"path\":\"./new.txt\",\"content\":\"different\"}"),
      workingDirectory, &sameTarget, &sameTargetErr));
  CHECK(StrView_Eq(inside.GrantKey, sameTarget.GrantKey));

  ToolPermissionInfo missingTail = {};
  StrBuf missingTailErr = StrBuf_Make(a, 64);
  CHECK(DescribeToolPermission(
      a, edit, SV("{\"path\":\"new/dir/file.txt\",\"edits\":[]}"),
      workingDirectory, &missingTail, &missingTailErr));
  CHECK(missingTail.AutoAllow);

  ToolPermissionInfo outside = {};
  StrBuf outsideErr = StrBuf_Make(a, 64);
  CHECK(DescribeToolPermission(
      a, write, SV("{\"path\":\"../outside.txt\",\"content\":\"x\"}"),
      workingDirectory, &outside, &outsideErr));
  CHECK(!outside.AutoAllow);

  CHECK(Fs_PathContained(workingDirectory, workingDirectory));
  CHECK(Fs_PathContained(workingDirectory,
                         Fs_Join(a, workingDirectory, SV("sub"))));
  CHECK(!Fs_PathContained(workingDirectory,
                          Fs_Join(a, Fs_Parent(workingDirectory),
                                  SV("sibling"))));

  Tool shell = ShellTool();
  ToolPermissionInfo command = {};
  StrBuf commandErr = StrBuf_Make(a, 64);
  CHECK(DescribeToolPermission(a, shell, SV("{\"command\":\"  git   status  \"}"),
                               workingDirectory, &command, &commandErr));
  CHECK_ALL(command.AutoAllow,
            StrView_Eq(command.Action, SVL("Run git status")),
            StrView_Eq(command.GrantKey, SVL("shell\ngit status")));

  ToolPermissionInfo chain = {};
  StrBuf chainErr = StrBuf_Make(a, 64);
  CHECK(DescribeToolPermission(
      a, shell, SV("{\"command\":\"git status && git diff\"}"),
      workingDirectory, &chain, &chainErr));
  CHECK(chain.AutoAllow);

  const char* reviewerCommands[] = {
      "git status && rm -rf x", "git status | head", "git status > out",
      "git status; git diff", "git status &", "git status $(pwd)",
      "git status 'x'", "git diff --output=changed.txt",
      "git show --ext-diff", "unknown-command", "git branch"};
  for (u32 i = 0; i < (u32)(sizeof(reviewerCommands) / sizeof(reviewerCommands[0]));
       i++) {
    StrBuf json = StrBuf_Make(a, 128);
    StrBuf_AppendCStr(&json, "{\"command\":\"");
    StrBuf_AppendCStr(&json, reviewerCommands[i]);
    StrBuf_AppendCStr(&json, "\"}");
    ToolPermissionInfo rejected = {};
    StrBuf rejectedErr = StrBuf_Make(a, 64);
    CHECK(DescribeToolPermission(a, shell, StrBuf_View(&json),
                                 workingDirectory, &rejected, &rejectedErr));
    CHECK(!rejected.AutoAllow);
  }

  ToolPermissionInfo help = {};
  StrBuf helpErr = StrBuf_Make(a, 64);
  CHECK(DescribeToolPermission(a, shell, SV("{\"command\":\"python --version\"}"),
                               workingDirectory, &help, &helpErr));
  CHECK(help.AutoAllow);

  ToolPermissionInfo gitHelp = {};
  StrBuf gitHelpErr = StrBuf_Make(a, 64);
  CHECK(DescribeToolPermission(a, shell,
                               SV("{\"command\":\"git --version\"}"),
                               workingDirectory, &gitHelp, &gitHelpErr));
  CHECK(gitHelp.AutoAllow);

  ToolPermissionInfo branch = {};
  StrBuf branchErr = StrBuf_Make(a, 64);
  CHECK(DescribeToolPermission(
      a, shell, SV("{\"command\":\"git branch --show-current\"}"),
      workingDirectory, &branch, &branchErr));
  CHECK(branch.AutoAllow);

  CLANKER_RMDIR("clanker_tool_permission_root/sub");
  CLANKER_RMDIR(root);
}

// ---- read ----------------------------------------------------------------

static void TestRead(Arena* a) {
  const struct {
    StrView Extension;
    StrView Mime;
  } images[] = {
      {SVL(".png"), SVL("image/png")}, {SVL(".PNG"), SVL("image/png")},
      {SVL(".jpg"), SVL("image/jpeg")}, {SVL(".JpEg"), SVL("image/jpeg")},
      {SVL(".GIF"), SVL("image/gif")}, {SVL(".WeBp"), SVL("image/webp")},
      {SVL(".BMP"), SVL("image/bmp")}, {StrView{}, StrView{}},
      {SVL(".png-extra"), StrView{}}, {SVL(".svg"), StrView{}},
  };
  for (u32 i = 0; i < sizeof(images) / sizeof(images[0]); i++)
    CHECK(StrView_Eq(Img_MimeForExt(images[i].Extension), images[i].Mime));

  const char* path = "clanker_tool_read_test.txt";
  MakeFile(path, "line1\nline2\n");

  Tool rt = ReadTool();
  ToolResult out = {};
  StrBuf err = StrBuf_Make(a, 32);
  bool ok = RunTool(rt, a, Args1(a, "path", path), &out, &err);
  CHECK_ALL(ok, out.Content.Count == 1,
            StrView_EqCStr(out.Content.Data[0].Text, "line1\nline2"));
  remove(path);

  // missing path + nonexistent file => errors
  ToolResult o2 = {};
  StrBuf e2 = StrBuf_Make(a, 32);
  CHECK(!RunTool(rt, a, SV("{}"), &o2, &e2));
  ToolResult o3 = {};
  StrBuf e3 = StrBuf_Make(a, 32);
  CHECK(!RunTool(rt, a, SV("{\"path\":\"no_such_file_xyz.txt\"}"), &o3, &e3));

  // POSIX can open a directory as a FILE but must not report a failed read as
  // a successful empty file.
  ToolResult directoryResult = {};
  StrBuf directoryError = StrBuf_Make(a, 64);
  CHECK(!RunTool(rt, a, SVL("{\"path\":\".\"}"), &directoryResult, &directoryError));
  CHECK_ALL(directoryResult.Content.Count == 0, directoryError.Length > 0);

  // offset + limit window with a continuation notice
  const char* p2 = "clanker_tool_read_ol.txt";
  MakeFile(p2, "l1\nl2\nl3\nl4\nl5\n");
  ToolResult o4 = {};
  StrBuf e4 = StrBuf_Make(a, 32);
  CHECK(RunTool(rt, a, SV("{\"path\":\"clanker_tool_read_ol.txt\",\"offset\":2,\"limit\":2}"), &o4, &e4));
  CHECK_ALL(StrView_Contains(o4.Content.Data[0].Text, "l2"),
            StrView_Contains(o4.Content.Data[0].Text, "l3"),
            !StrView_Contains(o4.Content.Data[0].Text, "l5"),
            StrView_Contains(o4.Content.Data[0].Text, "more lines in file"));
  remove(p2);

  // image file => text + image content blocks
  const char* pimg = "clanker_tool_read.png";
  unsigned char png[12] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13};
  {
    FILE* f = fopen(pimg, "wb");
    if (f) {
      fwrite(png, 1, sizeof(png), f);
      fclose(f);
    }
  }
  ToolResult o5 = {};
  StrBuf e5 = StrBuf_Make(a, 32);
  CHECK(RunTool(rt, a, Args1(a, "path", pimg), &o5, &e5));
  CHECK_ALL(o5.Content.Count == 2,
            StrView_Contains(o5.Content.Data[0].Text, "image/png"),
            o5.Content.Data[1].Kind == ContentKind::Image,
            StrView_EqCStr(o5.Content.Data[1].ImageMimeType, "image/png"),
            o5.Content.Data[1].ImageData.Length > 0);
  remove(pimg);
}

static void TestReadTruncation(Arena* a) {
  struct ReadCase {
    u32 LineBytes;
    u32 Lines;
    u32 Offset;
    u32 Limit;
    u32 KeptBytes;
    const char* Notice;
  };
  const ReadCase cases[] = {
      {1, 2001, 1, 0, 4000,
       "\n\n[Showing lines 1-2000 of 2001. Use offset=2001 to continue.]"},
      {1023, 60, 2, 0, 51200,
       "\n\n[Showing lines 2-51 of 60 (50.0KB limit). Use offset=52 to continue.]"},
      {61440, 3, 2, 1, 51200,
       "\n\n[Line 2 is 60.0KB, exceeds 50.0KB limit. Use shell: sed -n '2p' "
       "clanker_tool_read_truncation.txt | head -c 51200]"},
  };
  const char* path = "clanker_tool_read_truncation.txt";
  for (u32 i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    const ReadCase& test = cases[i];
    StrBuf input = StrBuf_Make(a, (test.LineBytes + 1) * test.Lines + 1);
    for (u32 line = 0; line < test.Lines; line++) {
      for (u32 column = 0; column < test.LineBytes; column++)
        StrBuf_AppendChar(&input, 'x');
      StrBuf_AppendChar(&input, '\n');
    }
    MakeFile(path, input.Data);
    StrBuf args = StrBuf_Make(a, 128);
    StrBuf_AppendCStr(&args, "{\"path\":\"clanker_tool_read_truncation.txt\",\"offset\":");
    StrBuf_AppendU64(&args, test.Offset);
    StrBuf_AppendCStr(&args, ",\"limit\":");
    StrBuf_AppendU64(&args, test.Limit);
    StrBuf_AppendChar(&args, '}');
    ToolResult out = {};
    StrBuf err = StrBuf_Make(a, 64);
    CHECK(RunTool(ReadTool(), a, StrBuf_View(&args), &out, &err));
    CHECK(out.Content.Count == 1);
    if (out.Content.Count == 1) {
      StrView text = out.Content.Data[0].Text;
      CHECK(text.Length == test.KeptBytes + strlen(test.Notice));
      if (text.Length >= test.KeptBytes) {
        CHECK(StrView_Eq(StrView{text.Data, test.KeptBytes},
                         StrView{input.Data + (test.Offset - 1) * (test.LineBytes + 1),
                                 test.KeptBytes}));
        CHECK(StrView_EqCStr(StrView{text.Data + test.KeptBytes,
                                    text.Length - test.KeptBytes}, test.Notice));
      }
    }
    remove(path);
  }
}

// ---- shell ---------------------------------------------------------------

// The path the truncation notice reports, i.e. everything between
// "Full output: " and the closing bracket.
static StrView ReportedOverflowPath(StrView text) {
  const char* marker = "Full output: ";
  u32 markerLength = (u32)strlen(marker);
  if (text.Length < markerLength) return StrView{};
  for (u32 i = 0; i + markerLength <= text.Length; i++) {
    if (memcmp(text.Data + i, marker, markerLength) != 0) continue;
    u32 start = i + markerLength;
    for (u32 end = start; end < text.Length; end++)
      if (text.Data[end] == ']') return StrView{text.Data + start, end - start};
    return StrView{};
  }
  return StrView{};
}

static void RemoveEmptyDir(const char* path) {
  CLANKER_RMDIR(path);
}

// Output past the truncation limit spills to a file whose path is reported back
// to the model. That file belongs to the session directory when there is one.
static void TestShellOverflow(Arena* a) {
  const char* source = "clanker_tool_shell_big.txt";
  {
    FILE* f = fopen(source, "wb");
    CHECK(f != nullptr);
    if (!f) return;
    // Comfortably past both the 2000-line and 50KB limits.
    for (u32 i = 0; i < 3000; i++) fprintf(f, "line %u padding padding\n", i);
    fclose(f);
  }
#if defined(_WIN32)
  StrView command = SVL("{\"command\":\"type clanker_tool_shell_big.txt\"}");
#else
  StrView command = SVL("{\"command\":\"cat clanker_tool_shell_big.txt\"}");
#endif

  const char* sessionDir = "clanker_tool_shell_session";
  ShellToolContext context = {};
  context.Directory = SV(sessionDir);
  Tool scoped = ShellTool(&context);

  ToolResult out = {};
  StrBuf err = StrBuf_Make(a, 64);
  CHECK(RunTool(scoped, a, command, &out, &err));
  CHECK(out.Content.Count == 1);
  StrView notice = out.Content.Data[0].Text;
  StrView logPath = ReportedOverflowPath(notice);
  // The directory is created on demand, and the spill lands inside it.
  CHECK_ALL(StrView_Contains(notice, "Showing lines"), logPath.Length > 0,
            StrView_Contains(logPath, sessionDir),
            StrView_Contains(logPath, "shell-"), Fs_IsDir(SV(sessionDir)));

  StrBuf logNative = StrBuf_Make(a, logPath.Length + 1);
  StrBuf_Append(&logNative, logPath);
  StrView spilled = Slurp(a, StrBuf_CStr(&logNative));
  // The spill holds the whole output, not the truncated view.
  CHECK_ALL(spilled.Length > TRUNC_MAX_BYTES,
            StrView_Contains(spilled, "line 0 padding"),
            StrView_Contains(spilled, "line 2999 padding"));
  remove(StrBuf_CStr(&logNative));
  RemoveEmptyDir(sessionDir);
  CHECK(!Fs_Exists(SV(sessionDir)));

  // With no session directory the tool falls back to the OS temp directory.
  Tool unscoped = ShellTool();
  ToolResult fallback = {};
  StrBuf fallbackErr = StrBuf_Make(a, 64);
  CHECK(RunTool(unscoped, a, command, &fallback, &fallbackErr));
  StrView fallbackPath = ReportedOverflowPath(fallback.Content.Data[0].Text);
  CHECK_ALL(fallbackPath.Length > 0,
            !StrView_Contains(fallbackPath, sessionDir),
            StrView_Contains(fallbackPath, "clanker-shell-"));
  StrBuf fallbackNative = StrBuf_Make(a, fallbackPath.Length + 1);
  StrBuf_Append(&fallbackNative, fallbackPath);
  CHECK(Fs_Exists(StrBuf_View(&fallbackNative)));
  remove(StrBuf_CStr(&fallbackNative));

  remove(source);
}

static void TestShell(Arena* a) {
  Tool shell = ShellTool();
  CHECK_ALL(StrView_Eq(shell.Name, SVL("shell")), StrView_Eq(shell.Label, SVL("Shell")));

  ToolResult out = {};
  StrBuf err = StrBuf_Make(a, 32);
  bool ok = RunTool(shell, a, SV("{\"command\":\"echo hello123\"}"), &out, &err);
  CHECK_ALL(ok, out.Content.Count == 1,
            StrView_Contains(out.Content.Data[0].Text, "hello123"),
            !StrView_Contains(out.Content.Data[0].Text, "exit code"));

  struct UpdateCapture {
    StrBuf Last;
    u32 Count;
    bool SawEmpty;
  } capture = {StrBuf_Make(a, 64), 0, false};
  bool accepting = true;
  ToolUpdateSink updates = {};
  updates.User = &capture;
  updates.Accepting = &accepting;
  updates.OnUpdate = [](void* user, const ToolResult* partial) {
    UpdateCapture* capture = (UpdateCapture*)user;
    capture->Count++;
    StrBuf_Clear(&capture->Last);
    StrView text = {};
    if (partial->Content.Count) text = partial->Content.Data[0].Text;
    if (text.Length == 0) capture->SawEmpty = true;
    StrBuf_Append(&capture->Last, text);
  };
  ToolResult streamed = {};
  StrBuf streamedErr = StrBuf_Make(a, 32);
  CHECK(RunTool(shell, a, SV("{\"command\":\"echo stream123\"}"),
                &streamed, &streamedErr, &updates));
  CHECK_ALL(capture.Count >= 2, capture.SawEmpty,
            StrView_Contains(StrBuf_View(&capture.Last), "stream123"));

  // non-zero exit => error result
  ToolResult o2 = {};
  StrBuf e2 = StrBuf_Make(a, 32);
  bool ok2 = RunTool(shell, a, SV("{\"command\":\"exit 3\"}"), &o2, &e2);
  CHECK_ALL(!ok2, StrView_Contains(StrBuf_View(&e2), "exited with code 3"));

  StrBuf directOutput = StrBuf_Make(a, 64);
  StrView directArgs[] = {SVL("--echo-arg"), SVL("spaces \" & | stay literal")};
  StrView testExecutable = Fs_RealPath(a, SV(g_test_executable));
  ExecArgvSink directSink = {};
  directSink.User = &directOutput;
  directSink.OnStdout = CaptureChunk;
  ExecArgvResult direct =
      Exec_RunArgv(a, testExecutable, directArgs, 2, 10000, nullptr, directSink);
  CHECK_ALL(direct.Spawned, direct.ExitCode == 0,
            StrView_EqCStr(StrBuf_View(&directOutput), "spaces \" & | stay literal"));
}

// ---- write ---------------------------------------------------------------

static void TestWrite(Arena* a) {
  Tool wt = WriteTool();
  const char* path = "clanker_tool_write_test.txt";
  ToolResult out = {};
  StrBuf err = StrBuf_Make(a, 32);
  bool ok = RunTool(wt, a, SV("{\"path\":\"clanker_tool_write_test.txt\",\"content\":\"written-data\"}"),
                    &out, &err);
  CHECK_ALL(ok, StrView_Contains(out.Content.Data[0].Text, "Successfully wrote"),
            StrView_EqCStr(Slurp(a, path), "written-data"));
  remove(path);
}

// ---- edit ----------------------------------------------------------------

static void TestEdit(Arena* a) {
  Tool et = EditTool();

  // exact match
  const char* p1 = "clanker_tool_edit_exact.txt";
  MakeFile(p1, "hello world\nsecond line\n");
  ToolResult o1 = {};
  StrBuf e1 = StrBuf_Make(a, 32);
  bool ok1 = RunTool(
      et, a,
      SV("{\"path\":\"clanker_tool_edit_exact.txt\",\"edits\":[{\"oldText\":\"hello world\",\"newText\":"
         "\"HELLO\"}]}"),
      &o1, &e1);
  CHECK_ALL(ok1, StrView_EqCStr(Slurp(a, p1), "HELLO\nsecond line\n"));
  remove(p1);

  ToolResult missingPath = {};
  StrBuf missingPathErr = StrBuf_Make(a, 32);
  CHECK(!RunTool(
      et, a, SV("{\"oldText\":\"hello\",\"newText\":\"goodbye\"}"),
      &missingPath, &missingPathErr));
  CHECK(StrView_Contains(StrBuf_View(&missingPathErr), "root.path"));

  // fuzzy match (different internal whitespace)
  const char* p2 = "clanker_tool_edit_fuzzy.txt";
  MakeFile(p2, "x = foo   bar\n");
  ToolResult o2 = {};
  StrBuf e2 = StrBuf_Make(a, 32);
  bool ok2 = RunTool(
      et, a,
      SV("{\"path\":\"clanker_tool_edit_fuzzy.txt\",\"edits\":[{\"oldText\":\"foo bar\",\"newText\":"
         "\"baz\"}]}"),
      &o2, &e2);
  CHECK_ALL(ok2, StrView_Contains(Slurp(a, p2), "baz"));
  remove(p2);

  // not-found => error
  const char* p3 = "clanker_tool_edit_err.txt";
  MakeFile(p3, "alpha\n");
  ToolResult o3 = {};
  StrBuf e3 = StrBuf_Make(a, 32);
  bool ok3 = RunTool(
      et, a,
      SV("{\"path\":\"clanker_tool_edit_err.txt\",\"edits\":[{\"oldText\":\"zzz\",\"newText\":\"q\"}]}"),
      &o3, &e3);
  CHECK(!ok3);
  remove(p3);
}

// ---- ls / find / grep (share a temp tree) --------------------------------

static void TestTreeTools(Arena* a) {
  Fs_MkdirP(a, SV("clanker_tools_it/sub"));
  MakeFile("clanker_tools_it/a.txt", "alpha\nbeta\ngamma\n");
  MakeFile("clanker_tools_it/b.md", "hello\n");
  MakeFile("clanker_tools_it/sub/c.txt", "deep\n");
  {
    FILE* longFile = fopen("clanker_tools_it/long.txt", "wb");
    if (longFile) {
      for (u32 i = 0; i < 600; i++) fputc('x', longFile);
      fputc('\n', longFile);
      fclose(longFile);
    }
  }

  // ls
  Tool lt = LsTool();
  ToolResult lo = {};
  StrBuf le = StrBuf_Make(a, 32);
  CHECK(RunTool(lt, a, Args1(a, "path", "clanker_tools_it"), &lo, &le));
  CHECK_ALL(StrView_Contains(lo.Content.Data[0].Text, "a.txt"),
            StrView_Contains(lo.Content.Data[0].Text, "b.md"),
            StrView_Contains(lo.Content.Data[0].Text, "sub/"));

  // find *.txt (basename match, recursive)
  Tool ft = FindTool();
  ToolResult fo = {};
  StrBuf fe = StrBuf_Make(a, 32);
  CHECK(RunTool(ft, a, SV("{\"pattern\":\"*.txt\",\"path\":\"clanker_tools_it\"}"), &fo, &fe));
  CHECK_ALL(StrView_Contains(fo.Content.Data[0].Text, "a.txt"),
            StrView_Contains(fo.Content.Data[0].Text, "c.txt"),
            !StrView_Contains(fo.Content.Data[0].Text, "b.md"));

  const char* oldRgPathRaw = getenv("CLANKER_RG_PATH");
  const char* oldFakeRgRaw = getenv("CLANKER_TEST_FAKE_RG");
  StrView oldRgPath = oldRgPathRaw ? StrView_Dup(a, SV(oldRgPathRaw)) : StrView{};
  StrView oldFakeRg = oldFakeRgRaw ? StrView_Dup(a, SV(oldFakeRgRaw)) : StrView{};
  bool useRealRipgrep = getenv("CLANKER_TEST_REAL_RG") != nullptr;
  if (!useRealRipgrep) {
    StrView fakeRg = Fs_RealPath(a, SV(g_test_executable));
    StrBuf fakeRgNative = StrBuf_Make(a, fakeRg.Length + 1);
    StrBuf_Append(&fakeRgNative, fakeRg);
    SetTestEnv("CLANKER_RG_PATH", StrBuf_CStr(&fakeRgNative));
    SetTestEnv("CLANKER_TEST_FAKE_RG", "1");
  }

  // grep forwards pi's ripgrep options and formats context from the source file.
  Tool gt = GrepTool();
  ToolResult go = {};
  StrBuf ge = StrBuf_Make(a, 32);
  CHECK(RunTool(
      gt, a,
      SV("{\"pattern\":\"BETA\",\"path\":\"clanker_tools_it\",\"glob\":\"a.txt\","
         "\"ignoreCase\":true,\"literal\":true,\"context\":1}"),
      &go, &ge));
  CHECK(StrView_EqCStr(go.Content.Data[0].Text,
                       "a.txt-1- alpha\na.txt:2: beta\na.txt-3- gamma"));

  ToolResult limited = {};
  StrBuf limitedErr = StrBuf_Make(a, 32);
  CHECK(RunTool(gt, a,
                SV("{\"pattern\":\"alpha|beta|gamma\",\"path\":\"clanker_tools_it/a.txt\","
                   "\"limit\":2}"),
                &limited, &limitedErr));
  CHECK_ALL(StrView_Contains(limited.Content.Data[0].Text, "a.txt:1: alpha"),
            StrView_Contains(limited.Content.Data[0].Text, "a.txt:2: beta"),
            !StrView_Contains(limited.Content.Data[0].Text, "gamma"),
            StrView_Contains(limited.Content.Data[0].Text, "2 matches limit reached"));

  ToolResult longLine = {};
  StrBuf longErr = StrBuf_Make(a, 32);
  CHECK(RunTool(gt, a, SV("{\"pattern\":\"x+\",\"path\":\"clanker_tools_it/long.txt\"}"),
                &longLine, &longErr));
  CHECK_ALL(StrView_Contains(longLine.Content.Data[0].Text, "... [truncated]"),
            StrView_Contains(longLine.Content.Data[0].Text,
                             "Some lines truncated to 500 chars"));

  ToolResult noMatches = {};
  StrBuf noMatchesErr = StrBuf_Make(a, 32);
  CHECK(RunTool(gt, a,
                SV("{\"pattern\":\"definitely-no-match\",\"path\":\"clanker_tools_it/a.txt\"}"),
                &noMatches, &noMatchesErr));
  CHECK(StrView_EqCStr(noMatches.Content.Data[0].Text, "No matches found"));

  ToolResult invalidRegex = {};
  StrBuf invalidRegexErr = StrBuf_Make(a, 32);
  CHECK(!RunTool(gt, a, SV("{\"pattern\":\"[\",\"path\":\"clanker_tools_it/a.txt\"}"),
                 &invalidRegex, &invalidRegexErr));
  CHECK(StrView_Contains(StrBuf_View(&invalidRegexErr), "regex parse error"));

  ToolResult missingPath = {};
  StrBuf missingPathErr = StrBuf_Make(a, 32);
  CHECK(!RunTool(gt, a, SV("{\"pattern\":\"x\",\"path\":\"clanker_tools_missing\"}"),
                 &missingPath, &missingPathErr));
  CHECK(StrView_Contains(StrBuf_View(&missingPathErr), "Path not found"));

  ToolResult emptyPattern = {};
  StrBuf emptyPatternErr = StrBuf_Make(a, 32);
  CHECK(RunTool(gt, a, SV("{\"pattern\":\"\",\"path\":\"clanker_tools_it/a.txt\"}"),
                &emptyPattern, &emptyPatternErr));
  CHECK(StrView_Contains(emptyPattern.Content.Data[0].Text, "a.txt:1: alpha"));

  StrBuf oldRgPathNative = StrBuf_Make(a, oldRgPath.Length + 1);
  StrBuf_Append(&oldRgPathNative, oldRgPath);
  StrBuf oldFakeRgNative = StrBuf_Make(a, oldFakeRg.Length + 1);
  StrBuf_Append(&oldFakeRgNative, oldFakeRg);
  SetTestEnv("CLANKER_TEST_FAKE_RG",
             oldFakeRg.Length ? StrBuf_CStr(&oldFakeRgNative) : nullptr);
  SetTestEnv("CLANKER_RG_PATH", oldRgPath.Length ? StrBuf_CStr(&oldRgPathNative) : nullptr);

  remove("clanker_tools_it/a.txt");
  remove("clanker_tools_it/b.md");
  remove("clanker_tools_it/long.txt");
  remove("clanker_tools_it/sub/c.txt");
  CLANKER_RMDIR("clanker_tools_it/sub");
  CLANKER_RMDIR("clanker_tools_it");
}

// ---- ask_user ------------------------------------------------------------

struct FakeAsk {
  StrView SeenQuestion;
  u32 SeenChoiceCount;
  u32 Calls;
  AgentAskStatus Reply;
  u32 ReplyIndex;
  StrView ReplyText;
  bool Fail;
};

static bool FakeAskFn(void* user, Arena* arena, const AgentAskRequest* request,
                      AgentAskResponse* out) {
  FakeAsk* fake = (FakeAsk*)user;
  fake->Calls++;
  fake->SeenQuestion = StrView_Dup(arena, request->Question);
  fake->SeenChoiceCount = request->ChoiceCount;
  if (fake->Fail) return false;
  out->Status = fake->Reply;
  out->ChoiceIndex = fake->ReplyIndex;
  out->Answer = StrView_Dup(
      arena, request->ChoiceCount && fake->Reply == AgentAskStatus::Answered
                 ? request->Choices[fake->ReplyIndex]
                 : fake->ReplyText);
  return true;
}

static void TestAskUser(Arena* a) {
  // No seam installed: the tool must report that nobody can answer rather than
  // block or fabricate a reply. This is the `-p` and offline-suite path.
  {
    Tool tool = AskUserTool(nullptr);
    ToolResult result = {};
    StrBuf err = StrBuf_Make(a, 128);
    CHECK(!RunTool(tool, a, SVL("{\"question\":\"Ship it?\"}"), &result, &err));
    CHECK(StrView_Contains(StrBuf_View(&err), "no user is available"));
  }

  FakeAsk fake = {};
  AskUserToolContext context = {};
  context.Ask.User = &fake;
  context.Ask.Ask = FakeAskFn;
  Tool tool = AskUserTool(&context);

  // Free-text question: the answer comes back verbatim as the tool result.
  {
    fake.Reply = AgentAskStatus::Answered;
    fake.ReplyText = SVL("use postgres");
    ToolResult result = {};
    StrBuf err = StrBuf_Make(a, 128);
    CHECK(RunTool(tool, a, SVL("{\"question\":\"Which database?\"}"), &result, &err));
    CHECK_ALL(fake.Calls == 1, fake.SeenChoiceCount == 0,
              StrView_Eq(fake.SeenQuestion, SVL("Which database?")));
    CHECK(result.Content.Count == 1);
    CHECK(StrView_Eq(result.Content.Data[0].Text, SVL("use postgres")));
  }

  // Choices are forwarded in order and the selection is echoed back.
  {
    fake = FakeAsk{};
    fake.Reply = AgentAskStatus::Answered;
    fake.ReplyIndex = 1;
    ToolResult result = {};
    StrBuf err = StrBuf_Make(a, 128);
    CHECK(RunTool(tool, a,
                  SVL("{\"question\":\"Pick one\",\"choices\":[\"a\",\"b\"]}"),
                  &result, &err));
    CHECK(fake.SeenChoiceCount == 2);
    CHECK(result.Content.Count == 1);
    CHECK(StrView_Eq(result.Content.Data[0].Text, SVL("b")));
  }

  // A dismissal is distinct from an unavailable seam, and both are errors the
  // model can read rather than a crash.
  {
    fake = FakeAsk{};
    fake.Reply = AgentAskStatus::Declined;
    ToolResult result = {};
    StrBuf err = StrBuf_Make(a, 128);
    CHECK(!RunTool(tool, a, SVL("{\"question\":\"Ship it?\"}"), &result, &err));
    CHECK(StrView_Contains(StrBuf_View(&err), "dismissed"));

    fake = FakeAsk{};
    fake.Fail = true;
    StrBuf failErr = StrBuf_Make(a, 128);
    CHECK(!RunTool(tool, a, SVL("{\"question\":\"Ship it?\"}"), &result, &failErr));
    CHECK(StrView_Contains(StrBuf_View(&failErr), "no user is available"));
  }

  // Malformed arguments are rejected before anyone is disturbed.
  {
    fake = FakeAsk{};
    ToolResult result = {};
    StrBuf err = StrBuf_Make(a, 128);
    CHECK(!RunTool(tool, a, SVL("{\"question\":\"\"}"), &result, &err));
    StrBuf err2 = StrBuf_Make(a, 128);
    CHECK(!RunTool(tool, a, SVL("{\"question\":\"q\",\"choices\":[\"a\",\"\"]}"),
                   &result, &err2));
    StrBuf err3 = StrBuf_Make(a, 128);
    CHECK(!RunTool(tool, a, SVL("{\"question\":\"q\",\"choices\":\"a\"}"), &result,
                   &err3));
    StrBuf err4 = StrBuf_Make(a, 128);
    CHECK(!RunTool(tool, a, SVL("{\"choices\":[\"a\"]}"), &result, &err4));
    StrBuf err5 = StrBuf_Make(a, 128);
    CHECK(!RunTool(tool, a,
                   SVL("{\"permission_request_id\":\"pending\",\"question\":\"q\"}"),
                   &result, &err5));
    CHECK(fake.Calls == 0);
  }

  // A `oneOf` parameter schema invites models to fill the unused branch with
  // explicit nulls. A null key is not an argument, so neither form may be
  // rejected for carrying one.
  {
    fake = FakeAsk{};
    fake.Reply = AgentAskStatus::Answered;
    fake.ReplyText = SVL("sqlite");
    ToolResult result = {};
    StrBuf err = StrBuf_Make(a, 128);
    CHECK(RunTool(tool, a,
                  SVL("{\"question\":\"Which database?\",\"choices\":null,"
                      "\"permission_request_id\":null}"),
                  &result, &err));
    CHECK_ALL(fake.Calls == 1, fake.SeenChoiceCount == 0,
              StrView_Eq(fake.SeenQuestion, SVL("Which database?")));
    CHECK(result.Content.Count == 1);
    CHECK(StrView_Eq(result.Content.Data[0].Text, SVL("sqlite")));

    // The permission branch reaches the engine rather than tripping the
    // mutual-exclusion check on its own null siblings.
    fake = FakeAsk{};
    StrBuf idErr = StrBuf_Make(a, 128);
    CHECK(!RunTool(tool, a,
                   SVL("{\"permission_request_id\":\"abc\",\"question\":null,"
                       "\"choices\":null}"),
                   &result, &idErr));
    CHECK(StrView_Contains(StrBuf_View(&idErr), "no permission engine"));
    CHECK(fake.Calls == 0);
  }

  // A batch must not put two questions on screen at once.
  CHECK(tool.ExecMode == ToolExecMode::Sequential);
}

void TestTools() {
  Arena a = ArenaCreate();
  TestBundledRipgrep(&a);
  TestToolPermissions(&a);
  TestRead(&a);
  TestReadTruncation(&a);
  TestShell(&a);
  TestShellOverflow(&a);
  TestWrite(&a);
  TestEdit(&a);
  TestTreeTools(&a);
  TestAskUser(&a);
  ArenaRelease(&a);
}
