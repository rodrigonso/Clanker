#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/arena.h"
#include "base/str.h"
#include "check.h"
#include "tools/exec.h"
#include "tools/fs.h"
#include "tools/tools.h"
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
    CHECK(fake.Calls == 0);
  }

  // A batch must not put two questions on screen at once.
  CHECK(tool.ExecMode == ToolExecMode::Sequential);
}

void TestTools() {
  Arena a = ArenaCreate();
  TestRead(&a);
  TestShell(&a);
  TestShellOverflow(&a);
  TestWrite(&a);
  TestEdit(&a);
  TestTreeTools(&a);
  TestAskUser(&a);
  ArenaRelease(&a);
}
