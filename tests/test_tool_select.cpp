#include "app/prompt_context.h"
#include "check.h"
#include "tools/tool_select.h"
#include "tools/tools.h"

namespace {

Tool Named(const char* name, StrView snippet) {
  Tool tool = {};
  tool.Name = SV(name);
  tool.PromptSnippet = snippet;
  return tool;
}

bool HasTool(const Tool* tools, u32 count, const char* name) {
  for (u32 i = 0; i < count; i++)
    if (StrView_EqCStr(tools[i].Name, name)) return true;
  return false;
}

void TestSplitNames(Arena* scratch) {
  Array<StrView> names = {};
  ToolSelect_SplitNames(scratch, SVL("read,edit"), &names);
  CHECK_ALL(names.Count == 2, StrView_EqCStr(names.Data[0], "read"),
            StrView_EqCStr(names.Data[1], "edit"));

  Array<StrView> messy = {};
  ToolSelect_SplitNames(scratch, SVL("  read , , \t edit ,"), &messy);
  CHECK_ALL(messy.Count == 2, StrView_EqCStr(messy.Data[0], "read"),
            StrView_EqCStr(messy.Data[1], "edit"));

  Array<StrView> empty = {};
  ToolSelect_SplitNames(scratch, SVL(",, ,"), &empty);
  CHECK(empty.Count == 0);

  Array<StrView> single = {};
  ToolSelect_SplitNames(scratch, SVL("shell"), &single);
  CHECK_ALL(single.Count == 1, StrView_EqCStr(single.Data[0], "shell"));

  ToolSelect_SplitNames(scratch, SVL("grep"), &single);
  CHECK_ALL(single.Count == 2, StrView_EqCStr(single.Data[1], "grep"));
}

void TestApply() {
  Tool all[] = {Named("read", SVL("Read files")), Named("write", SVL("Write files")),
                Named("edit", SVL("Edit files")), Named("shell", SVL("Run commands"))};
  constexpr u32 count = sizeof(all) / sizeof(all[0]);
  Tool out[count];

  u32 n = ToolSelect_Apply(all, count, nullptr, out);
  CHECK_ALL(n == count, StrView_EqCStr(out[0].Name, "read"),
            StrView_EqCStr(out[3].Name, "shell"));

  ToolSelection none = {};
  n = ToolSelect_Apply(all, count, &none, out);
  CHECK(n == count);

  StrView allow[] = {SVL("shell"), SVL("read")};
  ToolSelection allowOnly = {};
  allowOnly.Allow = allow;
  allowOnly.AllowCount = 2;
  n = ToolSelect_Apply(all, count, &allowOnly, out);
  CHECK_ALL(n == 2, StrView_EqCStr(out[0].Name, "read"),
            StrView_EqCStr(out[1].Name, "shell"));

  StrView exclude[] = {SVL("write"), SVL("shell")};
  ToolSelection excludeOnly = {};
  excludeOnly.Exclude = exclude;
  excludeOnly.ExcludeCount = 2;
  n = ToolSelect_Apply(all, count, &excludeOnly, out);
  CHECK_ALL(n == 2, StrView_EqCStr(out[0].Name, "read"),
            StrView_EqCStr(out[1].Name, "edit"));

  StrView both[] = {SVL("read"), SVL("shell")};
  ToolSelection conflicting = {};
  conflicting.Allow = both;
  conflicting.AllowCount = 2;
  conflicting.Exclude = both;
  conflicting.ExcludeCount = 2;
  CHECK(ToolSelect_Apply(all, count, &conflicting, out) == 0);

  ToolSelection allowThenExclude = {};
  allowThenExclude.Allow = both;
  allowThenExclude.AllowCount = 2;
  allowThenExclude.Exclude = exclude;
  allowThenExclude.ExcludeCount = 2;
  n = ToolSelect_Apply(all, count, &allowThenExclude, out);
  CHECK_ALL(n == 1, StrView_EqCStr(out[0].Name, "read"));

  ToolSelection nothing = {};
  nothing.Allow = allow;
  nothing.AllowCount = 2;
  nothing.NoTools = true;
  CHECK(ToolSelect_Apply(all, count, &nothing, out) == 0);
}

void TestUnknownNames(Arena* scratch) {
  Tool all[] = {Named("read", {}), Named("shell", {})};
  constexpr u32 count = sizeof(all) / sizeof(all[0]);

  StrView good[] = {SVL("read"), SVL("shell")};
  ToolSelection valid = {};
  valid.Allow = good;
  valid.AllowCount = 2;
  Array<StrView> unknown = {};
  ToolSelect_UnknownNames(scratch, all, count, &valid, &unknown);
  CHECK(unknown.Count == 0);

  StrView bad[] = {SVL("bogus"), SVL("read")};
  StrView alsoBad[] = {SVL("bogus"), SVL("nope")};
  ToolSelection invalid = {};
  invalid.Allow = bad;
  invalid.AllowCount = 2;
  invalid.Exclude = alsoBad;
  invalid.ExcludeCount = 2;
  Array<StrView> reported = {};
  ToolSelect_UnknownNames(scratch, all, count, &invalid, &reported);
  CHECK_ALL(reported.Count == 2, StrView_EqCStr(reported.Data[0], "bogus"),
            StrView_EqCStr(reported.Data[1], "nope"));

  StrView cased[] = {SVL("Read")};
  ToolSelection wrongCase = {};
  wrongCase.Allow = cased;
  wrongCase.AllowCount = 1;
  Array<StrView> casedOut = {};
  ToolSelect_UnknownNames(scratch, all, count, &wrongCase, &casedOut);
  CHECK(casedOut.Count == 1);
}

void TestPromptReflectsSelection(Arena* scratch) {
  Tool all[] = {Named("read", SVL("Read file contents")),
                Named("shell", SVL("Execute shell commands")),
                Named("write", SVL("Write files"))};
  constexpr u32 count = sizeof(all) / sizeof(all[0]);
  Tool selected[count];

  StrView exclude[] = {SVL("write")};
  ToolSelection selection = {};
  selection.Exclude = exclude;
  selection.ExcludeCount = 1;
  u32 n = ToolSelect_Apply(all, count, &selection, selected);
  CHECK_ALL(n == 2, HasTool(selected, n, "read"), !HasTool(selected, n, "write"));

  PromptContextOptions options = {};
  options.WorkingDirectory = SVL(".");
  options.Tools = selected;
  options.ToolCount = n;
  options.NoContextFiles = true;
  options.NoSkills = true;
  PromptContext context = {};
  StrBuf error = StrBuf_Make(scratch, 128);
  CHECK(PromptContext_Build(&options, &context, &error));
  CHECK_ALL(StrView_Contains(context.SystemPrompt, "- read: Read file contents"),
            StrView_Contains(context.SystemPrompt, "- shell: Execute shell commands"),
            !StrView_Contains(context.SystemPrompt, "Write files"));
  PromptContext_Release(&context);

  ToolSelection nothing = {};
  nothing.NoTools = true;
  u32 empty = ToolSelect_Apply(all, count, &nothing, selected);
  CHECK(empty == 0);
  options.Tools = selected;
  options.ToolCount = empty;
  StrBuf_Clear(&error);
  CHECK(PromptContext_Build(&options, &context, &error));
  CHECK_ALL(StrView_Contains(context.SystemPrompt, "Available tools:\n(none)\n"),
            !StrView_Contains(context.SystemPrompt, "Read file contents"));
  PromptContext_Release(&context);
}

void TestBuiltinCatalog() {
  ShellToolContext shellContext = {};
  Tool all[] = {ReadTool(), WriteTool(), EditTool(),        LsTool(),
                FindTool(), GrepTool(), ShellTool(&shellContext)};
  constexpr u32 count = sizeof(all) / sizeof(all[0]);
  CHECK_ALL(count == 7, HasTool(all, count, "read"), HasTool(all, count, "write"),
            HasTool(all, count, "edit"), HasTool(all, count, "ls"),
            HasTool(all, count, "find"), HasTool(all, count, "grep"),
            HasTool(all, count, "shell"),
            !HasTool(all, count, "bash"));
}

}  // namespace

void TestToolSelect() {
  Arena scratch = ArenaCreate((usize)16 << 20);
  TestSplitNames(&scratch);
  TestApply();
  TestUnknownNames(&scratch);
  TestPromptReflectsSelection(&scratch);
  TestBuiltinCatalog();
  ArenaRelease(&scratch);
}
