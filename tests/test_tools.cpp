#include <stdio.h>
#include <string.h>

#include "base/arena.h"
#include "base/str.h"
#include "check.h"
#include "tools/fs.h"
#include "tools/tools.h"

#if defined(_WIN32)
#include <direct.h>
#define MA_RMDIR _rmdir
#else
#include <unistd.h>
#define MA_RMDIR rmdir
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
  const char* path = "ma_tool_read_test.txt";
  MakeFile(path, "line1\nline2\n");

  Tool rt = ReadTool();
  ToolResult out = {};
  StrBuf err = StrBuf_Make(a, 32);
  bool ok = RunTool(rt, a, Args1(a, "path", path), &out, &err);
  CHECK(ok);
  CHECK(out.Content.Count == 1);
  CHECK(StrView_EqCStr(out.Content.Data[0].Text, "line1\nline2"));
  remove(path);

  // missing path + nonexistent file => errors
  ToolResult o2 = {};
  StrBuf e2 = StrBuf_Make(a, 32);
  CHECK(!RunTool(rt, a, SV("{}"), &o2, &e2));
  ToolResult o3 = {};
  StrBuf e3 = StrBuf_Make(a, 32);
  CHECK(!RunTool(rt, a, SV("{\"path\":\"no_such_file_xyz.txt\"}"), &o3, &e3));

  // offset + limit window with a continuation notice
  const char* p2 = "ma_tool_read_ol.txt";
  MakeFile(p2, "l1\nl2\nl3\nl4\nl5\n");
  ToolResult o4 = {};
  StrBuf e4 = StrBuf_Make(a, 32);
  CHECK(RunTool(rt, a, SV("{\"path\":\"ma_tool_read_ol.txt\",\"offset\":2,\"limit\":2}"), &o4, &e4));
  CHECK(StrView_Contains(o4.Content.Data[0].Text, "l2"));
  CHECK(StrView_Contains(o4.Content.Data[0].Text, "l3"));
  CHECK(!StrView_Contains(o4.Content.Data[0].Text, "l5"));
  CHECK(StrView_Contains(o4.Content.Data[0].Text, "more lines in file"));
  remove(p2);

  // image file => text + image content blocks
  const char* pimg = "ma_tool_read.png";
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
  CHECK(o5.Content.Count == 2);
  CHECK(StrView_Contains(o5.Content.Data[0].Text, "image/png"));
  CHECK(o5.Content.Data[1].Kind == ContentKind::Image);
  CHECK(StrView_EqCStr(o5.Content.Data[1].ImageMimeType, "image/png"));
  CHECK(o5.Content.Data[1].ImageData.Length > 0);
  remove(pimg);
}

// ---- bash ----------------------------------------------------------------

static void TestBash(Arena* a) {
  Tool bt = BashTool();

  ToolResult out = {};
  StrBuf err = StrBuf_Make(a, 32);
  bool ok = RunTool(bt, a, SV("{\"command\":\"echo hello123\"}"), &out, &err);
  CHECK(ok);
  CHECK(out.Content.Count == 1);
  CHECK(StrView_Contains(out.Content.Data[0].Text, "hello123"));
  CHECK(!StrView_Contains(out.Content.Data[0].Text, "exit code"));

  // non-zero exit => error result
  ToolResult o2 = {};
  StrBuf e2 = StrBuf_Make(a, 32);
  bool ok2 = RunTool(bt, a, SV("{\"command\":\"exit 3\"}"), &o2, &e2);
  CHECK(!ok2);
  CHECK(StrView_Contains(StrBuf_View(&e2), "exited with code 3"));
}

// ---- write ---------------------------------------------------------------

static void TestWrite(Arena* a) {
  Tool wt = WriteTool();
  const char* path = "ma_tool_write_test.txt";
  ToolResult out = {};
  StrBuf err = StrBuf_Make(a, 32);
  bool ok = RunTool(wt, a, SV("{\"path\":\"ma_tool_write_test.txt\",\"content\":\"written-data\"}"),
                    &out, &err);
  CHECK(ok);
  CHECK(StrView_Contains(out.Content.Data[0].Text, "Successfully wrote"));
  CHECK(StrView_EqCStr(Slurp(a, path), "written-data"));
  remove(path);
}

// ---- edit ----------------------------------------------------------------

static void TestEdit(Arena* a) {
  Tool et = EditTool();

  // exact match
  const char* p1 = "ma_tool_edit_exact.txt";
  MakeFile(p1, "hello world\nsecond line\n");
  ToolResult o1 = {};
  StrBuf e1 = StrBuf_Make(a, 32);
  bool ok1 = RunTool(
      et, a,
      SV("{\"path\":\"ma_tool_edit_exact.txt\",\"edits\":[{\"oldText\":\"hello world\",\"newText\":"
         "\"HELLO\"}]}"),
      &o1, &e1);
  CHECK(ok1);
  CHECK(StrView_EqCStr(Slurp(a, p1), "HELLO\nsecond line\n"));
  remove(p1);

  // fuzzy match (different internal whitespace)
  const char* p2 = "ma_tool_edit_fuzzy.txt";
  MakeFile(p2, "x = foo   bar\n");
  ToolResult o2 = {};
  StrBuf e2 = StrBuf_Make(a, 32);
  bool ok2 = RunTool(
      et, a,
      SV("{\"path\":\"ma_tool_edit_fuzzy.txt\",\"edits\":[{\"oldText\":\"foo bar\",\"newText\":"
         "\"baz\"}]}"),
      &o2, &e2);
  CHECK(ok2);
  CHECK(StrView_Contains(Slurp(a, p2), "baz"));
  remove(p2);

  // not-found => error
  const char* p3 = "ma_tool_edit_err.txt";
  MakeFile(p3, "alpha\n");
  ToolResult o3 = {};
  StrBuf e3 = StrBuf_Make(a, 32);
  bool ok3 = RunTool(
      et, a,
      SV("{\"path\":\"ma_tool_edit_err.txt\",\"edits\":[{\"oldText\":\"zzz\",\"newText\":\"q\"}]}"),
      &o3, &e3);
  CHECK(!ok3);
  remove(p3);
}

// ---- ls / find / grep (share a temp tree) --------------------------------

static void TestTreeTools(Arena* a) {
  Fs_MkdirP(a, SV("ma_tools_it/sub"));
  MakeFile("ma_tools_it/a.txt", "alpha\nbeta\ngamma\n");
  MakeFile("ma_tools_it/b.md", "hello\n");
  MakeFile("ma_tools_it/sub/c.txt", "deep\n");

  // ls
  Tool lt = LsTool();
  ToolResult lo = {};
  StrBuf le = StrBuf_Make(a, 32);
  CHECK(RunTool(lt, a, Args1(a, "path", "ma_tools_it"), &lo, &le));
  CHECK(StrView_Contains(lo.Content.Data[0].Text, "a.txt"));
  CHECK(StrView_Contains(lo.Content.Data[0].Text, "b.md"));
  CHECK(StrView_Contains(lo.Content.Data[0].Text, "sub/"));

  // find *.txt (basename match, recursive)
  Tool ft = FindTool();
  ToolResult fo = {};
  StrBuf fe = StrBuf_Make(a, 32);
  CHECK(RunTool(ft, a, SV("{\"pattern\":\"*.txt\",\"path\":\"ma_tools_it\"}"), &fo, &fe));
  CHECK(StrView_Contains(fo.Content.Data[0].Text, "a.txt"));
  CHECK(StrView_Contains(fo.Content.Data[0].Text, "c.txt"));
  CHECK(!StrView_Contains(fo.Content.Data[0].Text, "b.md"));

  // grep
  Tool gt = GrepTool();
  ToolResult go = {};
  StrBuf ge = StrBuf_Make(a, 32);
  CHECK(RunTool(gt, a, SV("{\"pattern\":\"beta\",\"path\":\"ma_tools_it\"}"), &go, &ge));
  CHECK(StrView_Contains(go.Content.Data[0].Text, "beta"));
  CHECK(StrView_Contains(go.Content.Data[0].Text, "a.txt"));

  remove("ma_tools_it/a.txt");
  remove("ma_tools_it/b.md");
  remove("ma_tools_it/sub/c.txt");
  MA_RMDIR("ma_tools_it/sub");
  MA_RMDIR("ma_tools_it");
}

void TestTools() {
  Arena a = ArenaCreate();
  TestRead(&a);
  TestBash(&a);
  TestWrite(&a);
  TestEdit(&a);
  TestTreeTools(&a);
  ArenaRelease(&a);
}
