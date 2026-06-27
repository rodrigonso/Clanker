#include <stdio.h>

#include "base/arena.h"
#include "base/str.h"
#include "check.h"
#include "tools/tools.h"

void TestTools() {
  Arena a = ArenaCreate();

  // read tool
  {
    const char* path = "ma_tool_read_test.txt";
    FILE* f = fopen(path, "wb");
    fputs("line1\nline2\n", f);
    fclose(f);

    Tool rt = ReadTool();
    ToolResult out = {};
    StrBuf err = StrBuf_Make(&a, 32);
    StrBuf args = StrBuf_Make(&a, 64);
    StrBuf_AppendCStr(&args, "{\"path\":\"");
    StrBuf_AppendCStr(&args, path);
    StrBuf_AppendCStr(&args, "\"}");
    bool ok = rt.Execute(rt.Ctx, &a, SV("c1"), StrBuf_View(&args), nullptr, &out, &err);
    CHECK(ok);
    CHECK(out.Content.Count == 1);
    CHECK(StrView_EqCStr(out.Content.Data[0].Text, "line1\nline2\n"));
    remove(path);

    ToolResult o2 = {};
    StrBuf e2 = StrBuf_Make(&a, 32);
    CHECK(!rt.Execute(rt.Ctx, &a, SV("c2"), SV("{}"), nullptr, &o2, &e2));

    ToolResult o3 = {};
    StrBuf e3 = StrBuf_Make(&a, 32);
    CHECK(!rt.Execute(rt.Ctx, &a, SV("c3"), SV("{\"path\":\"no_such_file_xyz.txt\"}"), nullptr, &o3, &e3));
  }

  // bash tool
  {
    Tool bt = BashTool();
    ToolResult out = {};
    StrBuf err = StrBuf_Make(&a, 32);
    bool ok = bt.Execute(bt.Ctx, &a, SV("c1"), SV("{\"command\":\"echo hello123\"}"), nullptr, &out, &err);
    CHECK(ok);
    CHECK(out.Content.Count == 1);
    CHECK(StrView_Contains(out.Content.Data[0].Text, "hello123"));
    CHECK(StrView_Contains(out.Content.Data[0].Text, "exit code: 0"));
  }

  ArenaRelease(&a);
}
