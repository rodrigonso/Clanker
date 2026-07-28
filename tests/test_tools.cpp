#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/arena.h"
#include "base/str.h"
#include "check.h"
#include "tools/exec.h"
#include "tools/fs.h"
#include "tools/tools.h"
#include "tests.h"

#if defined(_WIN32)
#include <direct.h>
#define CLANK_RMDIR _rmdir
#else
#include <unistd.h>
#define CLANK_RMDIR rmdir
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

static bool RunTool(const Tool& t, Arena* a, StrView args, ToolResult* out, StrBuf* err) {
  return t.Execute(t.Ctx, a, SV("id"), args, nullptr, out, err);
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

static StrView Args1(Arena* a, const char* k, const char* v) {
  StrBuf b = StrBuf_Make(a, 64);
  StrBuf_AppendCStr(&b, "{\"");
  StrBuf_AppendCStr(&b, k);
  StrBuf_AppendCStr(&b, "\":\"");
  StrBuf_AppendCStr(&b, v);
  StrBuf_AppendCStr(&b, "\"}");
  return StrBuf_View(&b);
}

// ---- read ----------------------------------------------------------------

static void TestRead(Arena* a) {
  const char* path = "clank_tool_read_test.txt";
  MakeFile(path, "line1\nline2\n");

  Tool rt = ReadTool();
  CHECK(rt.ReplayPolicy == ToolReplayPolicy::ReplaySafe);
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

  // offset + limit window with a continuation notice
  const char* p2 = "clank_tool_read_ol.txt";
  MakeFile(p2, "l1\nl2\nl3\nl4\nl5\n");
  ToolResult o4 = {};
  StrBuf e4 = StrBuf_Make(a, 32);
  CHECK(RunTool(rt, a, SV("{\"path\":\"clank_tool_read_ol.txt\",\"offset\":2,\"limit\":2}"), &o4, &e4));
  CHECK_ALL(StrView_Contains(o4.Content.Data[0].Text, "l2"),
            StrView_Contains(o4.Content.Data[0].Text, "l3"),
            !StrView_Contains(o4.Content.Data[0].Text, "l5"),
            StrView_Contains(o4.Content.Data[0].Text, "more lines in file"));
  remove(p2);

  // image file => text + image content blocks
  const char* pimg = "clank_tool_read.png";
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

// ---- shell ---------------------------------------------------------------

static void TestShell(Arena* a) {
  Tool shell = ShellTool();
  CHECK_ALL(StrView_Eq(shell.Name, SVL("shell")), StrView_Eq(shell.Label, SVL("Shell")),
            shell.ReplayPolicy == ToolReplayPolicy::ManualReconcile);

  ToolResult out = {};
  StrBuf err = StrBuf_Make(a, 32);
  bool ok = RunTool(shell, a, SV("{\"command\":\"echo hello123\"}"), &out, &err);
  CHECK_ALL(ok, out.Content.Count == 1,
            StrView_Contains(out.Content.Data[0].Text, "hello123"),
            !StrView_Contains(out.Content.Data[0].Text, "exit code"));

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
  const char* path = "clank_tool_write_test.txt";
  ToolResult out = {};
  StrBuf err = StrBuf_Make(a, 32);
  bool ok = RunTool(wt, a, SV("{\"path\":\"clank_tool_write_test.txt\",\"content\":\"written-data\"}"),
                    &out, &err);
  CHECK_ALL(ok, StrView_Contains(out.Content.Data[0].Text, "Successfully wrote"),
            StrView_EqCStr(Slurp(a, path), "written-data"));
  remove(path);
}

// ---- edit ----------------------------------------------------------------

static void TestEdit(Arena* a) {
  Tool et = EditTool();

  // exact match
  const char* p1 = "clank_tool_edit_exact.txt";
  MakeFile(p1, "hello world\nsecond line\n");
  ToolResult o1 = {};
  StrBuf e1 = StrBuf_Make(a, 32);
  bool ok1 = RunTool(
      et, a,
      SV("{\"path\":\"clank_tool_edit_exact.txt\",\"edits\":[{\"oldText\":\"hello world\",\"newText\":"
         "\"HELLO\"}]}"),
      &o1, &e1);
  CHECK_ALL(ok1, StrView_EqCStr(Slurp(a, p1), "HELLO\nsecond line\n"));
  remove(p1);

  // fuzzy match (different internal whitespace)
  const char* p2 = "clank_tool_edit_fuzzy.txt";
  MakeFile(p2, "x = foo   bar\n");
  ToolResult o2 = {};
  StrBuf e2 = StrBuf_Make(a, 32);
  bool ok2 = RunTool(
      et, a,
      SV("{\"path\":\"clank_tool_edit_fuzzy.txt\",\"edits\":[{\"oldText\":\"foo bar\",\"newText\":"
         "\"baz\"}]}"),
      &o2, &e2);
  CHECK_ALL(ok2, StrView_Contains(Slurp(a, p2), "baz"));
  remove(p2);

  // not-found => error
  const char* p3 = "clank_tool_edit_err.txt";
  MakeFile(p3, "alpha\n");
  ToolResult o3 = {};
  StrBuf e3 = StrBuf_Make(a, 32);
  bool ok3 = RunTool(
      et, a,
      SV("{\"path\":\"clank_tool_edit_err.txt\",\"edits\":[{\"oldText\":\"zzz\",\"newText\":\"q\"}]}"),
      &o3, &e3);
  CHECK(!ok3);
  remove(p3);
}

// ---- ls / find / grep (share a temp tree) --------------------------------

static void TestTreeTools(Arena* a) {
  Fs_MkdirP(a, SV("clank_tools_it/sub"));
  MakeFile("clank_tools_it/a.txt", "alpha\nbeta\ngamma\n");
  MakeFile("clank_tools_it/b.md", "hello\n");
  MakeFile("clank_tools_it/sub/c.txt", "deep\n");
  {
    FILE* longFile = fopen("clank_tools_it/long.txt", "wb");
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
  CHECK(RunTool(lt, a, Args1(a, "path", "clank_tools_it"), &lo, &le));
  CHECK_ALL(StrView_Contains(lo.Content.Data[0].Text, "a.txt"),
            StrView_Contains(lo.Content.Data[0].Text, "b.md"),
            StrView_Contains(lo.Content.Data[0].Text, "sub/"));

  // find *.txt (basename match, recursive)
  Tool ft = FindTool();
  ToolResult fo = {};
  StrBuf fe = StrBuf_Make(a, 32);
  CHECK(RunTool(ft, a, SV("{\"pattern\":\"*.txt\",\"path\":\"clank_tools_it\"}"), &fo, &fe));
  CHECK_ALL(StrView_Contains(fo.Content.Data[0].Text, "a.txt"),
            StrView_Contains(fo.Content.Data[0].Text, "c.txt"),
            !StrView_Contains(fo.Content.Data[0].Text, "b.md"));

  const char* oldRgPathRaw = getenv("CLANK_RG_PATH");
  const char* oldFakeRgRaw = getenv("CLANK_TEST_FAKE_RG");
  StrView oldRgPath = oldRgPathRaw ? StrView_Dup(a, SV(oldRgPathRaw)) : StrView{};
  StrView oldFakeRg = oldFakeRgRaw ? StrView_Dup(a, SV(oldFakeRgRaw)) : StrView{};
  bool useRealRipgrep = getenv("CLANK_TEST_REAL_RG") != nullptr;
  if (!useRealRipgrep) {
    StrView fakeRg = Fs_RealPath(a, SV(g_test_executable));
    StrBuf fakeRgNative = StrBuf_Make(a, fakeRg.Length + 1);
    StrBuf_Append(&fakeRgNative, fakeRg);
    SetTestEnv("CLANK_RG_PATH", StrBuf_CStr(&fakeRgNative));
    SetTestEnv("CLANK_TEST_FAKE_RG", "1");
  }

  // grep forwards pi's ripgrep options and formats context from the source file.
  Tool gt = GrepTool();
  ToolResult go = {};
  StrBuf ge = StrBuf_Make(a, 32);
  CHECK(RunTool(
      gt, a,
      SV("{\"pattern\":\"BETA\",\"path\":\"clank_tools_it\",\"glob\":\"a.txt\","
         "\"ignoreCase\":true,\"literal\":true,\"context\":1}"),
      &go, &ge));
  CHECK(StrView_EqCStr(go.Content.Data[0].Text,
                       "a.txt-1- alpha\na.txt:2: beta\na.txt-3- gamma"));

  ToolResult limited = {};
  StrBuf limitedErr = StrBuf_Make(a, 32);
  CHECK(RunTool(gt, a,
                SV("{\"pattern\":\"alpha|beta|gamma\",\"path\":\"clank_tools_it/a.txt\","
                   "\"limit\":2}"),
                &limited, &limitedErr));
  CHECK_ALL(StrView_Contains(limited.Content.Data[0].Text, "a.txt:1: alpha"),
            StrView_Contains(limited.Content.Data[0].Text, "a.txt:2: beta"),
            !StrView_Contains(limited.Content.Data[0].Text, "gamma"),
            StrView_Contains(limited.Content.Data[0].Text, "2 matches limit reached"));

  ToolResult longLine = {};
  StrBuf longErr = StrBuf_Make(a, 32);
  CHECK(RunTool(gt, a, SV("{\"pattern\":\"x+\",\"path\":\"clank_tools_it/long.txt\"}"),
                &longLine, &longErr));
  CHECK_ALL(StrView_Contains(longLine.Content.Data[0].Text, "... [truncated]"),
            StrView_Contains(longLine.Content.Data[0].Text,
                             "Some lines truncated to 500 chars"));

  ToolResult noMatches = {};
  StrBuf noMatchesErr = StrBuf_Make(a, 32);
  CHECK(RunTool(gt, a,
                SV("{\"pattern\":\"definitely-no-match\",\"path\":\"clank_tools_it/a.txt\"}"),
                &noMatches, &noMatchesErr));
  CHECK(StrView_EqCStr(noMatches.Content.Data[0].Text, "No matches found"));

  ToolResult invalidRegex = {};
  StrBuf invalidRegexErr = StrBuf_Make(a, 32);
  CHECK(!RunTool(gt, a, SV("{\"pattern\":\"[\",\"path\":\"clank_tools_it/a.txt\"}"),
                 &invalidRegex, &invalidRegexErr));
  CHECK(StrView_Contains(StrBuf_View(&invalidRegexErr), "regex parse error"));

  ToolResult missingPath = {};
  StrBuf missingPathErr = StrBuf_Make(a, 32);
  CHECK(!RunTool(gt, a, SV("{\"pattern\":\"x\",\"path\":\"clank_tools_missing\"}"),
                 &missingPath, &missingPathErr));
  CHECK(StrView_Contains(StrBuf_View(&missingPathErr), "Path not found"));

  ToolResult emptyPattern = {};
  StrBuf emptyPatternErr = StrBuf_Make(a, 32);
  CHECK(RunTool(gt, a, SV("{\"pattern\":\"\",\"path\":\"clank_tools_it/a.txt\"}"),
                &emptyPattern, &emptyPatternErr));
  CHECK(StrView_Contains(emptyPattern.Content.Data[0].Text, "a.txt:1: alpha"));

  StrBuf oldRgPathNative = StrBuf_Make(a, oldRgPath.Length + 1);
  StrBuf_Append(&oldRgPathNative, oldRgPath);
  StrBuf oldFakeRgNative = StrBuf_Make(a, oldFakeRg.Length + 1);
  StrBuf_Append(&oldFakeRgNative, oldFakeRg);
  SetTestEnv("CLANK_TEST_FAKE_RG",
             oldFakeRg.Length ? StrBuf_CStr(&oldFakeRgNative) : nullptr);
  SetTestEnv("CLANK_RG_PATH", oldRgPath.Length ? StrBuf_CStr(&oldRgPathNative) : nullptr);

  remove("clank_tools_it/a.txt");
  remove("clank_tools_it/b.md");
  remove("clank_tools_it/long.txt");
  remove("clank_tools_it/sub/c.txt");
  CLANK_RMDIR("clank_tools_it/sub");
  CLANK_RMDIR("clank_tools_it");
}

void TestTools() {
  Arena a = ArenaCreate();
  TestRead(&a);
  TestShell(&a);
  TestWrite(&a);
  TestEdit(&a);
  TestTreeTools(&a);
  ArenaRelease(&a);
}
