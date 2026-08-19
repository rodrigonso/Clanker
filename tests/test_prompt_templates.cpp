#include <stdio.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "app/prompt_templates.h"
#include "base/config_file.h"
#include "check.h"
#include "tools/fs.h"

namespace {

void RemoveDir(const char* path) {
#if defined(_WIN32)
  _rmdir(path);
#else
  rmdir(path);
#endif
}

StrView Sub(Arena* a, const char* content, const char* argsString) {
  Array<StrView> args = {};
  PromptTemplates_SplitArgs(a, SV(argsString), &args);
  return PromptTemplates_Substitute(a, SV(content), args.Data, args.Count);
}

bool SubIs(Arena* a, const char* content, const char* argsString,
           const char* expected) {
  return StrView_EqCStr(Sub(a, content, argsString), expected);
}

void TestSplitArgs(Arena* a) {
  Array<StrView> args = {};
  PromptTemplates_SplitArgs(a, SVL("one two three"), &args);
  CHECK_ALL(args.Count == 3, StrView_EqCStr(args.Data[0], "one"),
            StrView_EqCStr(args.Data[2], "three"));

  Array<StrView> spaced = {};
  PromptTemplates_SplitArgs(a, SVL("  a \t\n b  "), &spaced);
  CHECK_ALL(spaced.Count == 2, StrView_EqCStr(spaced.Data[0], "a"),
            StrView_EqCStr(spaced.Data[1], "b"));

  Array<StrView> empty = {};
  PromptTemplates_SplitArgs(a, SVL("   "), &empty);
  CHECK(empty.Count == 0);

  Array<StrView> quoted = {};
  PromptTemplates_SplitArgs(a, SVL("\"two words\" 'and more' plain"), &quoted);
  CHECK_ALL(quoted.Count == 3, StrView_EqCStr(quoted.Data[0], "two words"),
            StrView_EqCStr(quoted.Data[1], "and more"),
            StrView_EqCStr(quoted.Data[2], "plain"));

  Array<StrView> glued = {};
  PromptTemplates_SplitArgs(a, SVL("pre\"mid dle\"post"), &glued);
  CHECK_ALL(glued.Count == 1, StrView_EqCStr(glued.Data[0], "premid dlepost"));

  Array<StrView> nested = {};
  PromptTemplates_SplitArgs(a, SVL("\"it's fine\""), &nested);
  CHECK_ALL(nested.Count == 1, StrView_EqCStr(nested.Data[0], "it's fine"));

  Array<StrView> blank = {};
  PromptTemplates_SplitArgs(a, SVL("a \"\" b"), &blank);
  CHECK_ALL(blank.Count == 3, StrView_EqCStr(blank.Data[0], "a"),
            blank.Data[1].Length == 0, StrView_EqCStr(blank.Data[2], "b"));

  Array<StrView> unterminated = {};
  PromptTemplates_SplitArgs(a, SVL("start \"never closed"), &unterminated);
  CHECK_ALL(unterminated.Count == 2,
            StrView_EqCStr(unterminated.Data[1], "never closed"));

  Array<StrView> backslash = {};
  PromptTemplates_SplitArgs(a, SVL("a\\ b"), &backslash);
  CHECK_ALL(backslash.Count == 2, StrView_EqCStr(backslash.Data[0], "a\\"));
}

void TestSubstitute(Arena* a) {
  CHECK_ALL(SubIs(a, "Review $1 then $2", "alpha beta", "Review alpha then beta"),
            SubIs(a, "All: $@", "a b c", "All: a b c"),
            SubIs(a, "All: $ARGUMENTS", "a b c", "All: a b c"));

  CHECK_ALL(SubIs(a, "[$1][$2]", "only", "[only][]"),
            SubIs(a, "[$1]", "", "[]"), SubIs(a, "[$@]", "", "[]"));

  CHECK(SubIs(a, "$12", "a b c", ""));
  CHECK(SubIs(a, "$1 2", "a b c", "a 2"));

  CHECK_ALL(SubIs(a, "cost is $ 5", "x", "cost is $ 5"),
            SubIs(a, "$FOO", "x", "$FOO"), SubIs(a, "trailing $", "x", "trailing $"),
            SubIs(a, "$ARGUMENT", "x", "$ARGUMENT"));

  CHECK(SubIs(a, "$ARGUMENTSX", "a b", "a bX"));

  CHECK_ALL(SubIs(a, "${1:-fallback}", "", "fallback"),
            SubIs(a, "${1:-fallback}", "given", "given"),
            SubIs(a, "${2:-second}", "one", "second"),
            SubIs(a, "${@:-nothing}", "", "nothing"),
            SubIs(a, "${@:-nothing}", "a b", "a b"),
            SubIs(a, "${ARGUMENTS:-nothing}", "", "nothing"));

  CHECK_ALL(SubIs(a, "[${1:-}]", "", "[]"),
            SubIs(a, "${1:-a b c}", "", "a b c"));

  CHECK(SubIs(a, "${1:-fallback}", "\"\"", "fallback"));

  CHECK_ALL(SubIs(a, "${@:2}", "a b c d", "b c d"),
            SubIs(a, "${@:1}", "a b c", "a b c"),
            SubIs(a, "${@:0}", "a b c", "a b c"),
            SubIs(a, "${@:2:2}", "a b c d e", "b c"),
            SubIs(a, "${@:1:1}", "a b c", "a"),
            SubIs(a, "${@:2:99}", "a b c", "b c"),
            SubIs(a, "${@:9}", "a b c", ""),
            SubIs(a, "${@:2:0}", "a b c", ""));

  CHECK_ALL(SubIs(a, "${}", "a", "${}"), SubIs(a, "${1}", "a", "${1}"),
            SubIs(a, "${1:-unclosed", "a", "${1:-unclosed"),
            SubIs(a, "${@:x}", "a", "${@:x}"),
            SubIs(a, "${nope:-x}", "a", "${nope:-x}"));

  CHECK(SubIs(a, "just do the thing", "these args vanish",
              "just do the thing"));

  CHECK_ALL(SubIs(a, "$1|$2", "\"two words\" second", "two words|second"),
            SubIs(a, "$@", "\"two words\" second", "two words second"));

  CHECK(SubIs(a, "", "a b", ""));
}

void TestParseFile(Arena* a) {
  PromptTemplate tmpl = {};
  CHECK(PromptTemplates_ParseFile(
      a, SVL("/tmp/prompts/review.md"),
      SVL("---\ndescription: Review a diff\nargument-hint: <path>\n---\nBody $1\n"),
      &tmpl));
  CHECK_ALL(StrView_EqCStr(tmpl.Name, "review"),
            StrView_EqCStr(tmpl.Description, "Review a diff"),
            StrView_EqCStr(tmpl.ArgumentHint, "<path>"),
            StrView_EqCStr(tmpl.Content, "Body $1"));

  PromptTemplate named = {};
  CHECK(PromptTemplates_ParseFile(a, SVL("/tmp/prompts/actual.md"),
                                  SVL("---\nname: other\n---\nBody"), &named));
  CHECK(StrView_EqCStr(named.Name, "actual"));

  PromptTemplate upper = {};
  CHECK(PromptTemplates_ParseFile(a, SVL("/tmp/prompts/Loud.MD"), SVL("Body"),
                                  &upper));
  CHECK(StrView_EqCStr(upper.Name, "Loud"));

  PromptTemplate win = {};
  CHECK(PromptTemplates_ParseFile(a, SVL("C:\\prompts\\ship.md"), SVL("Body"),
                                  &win));
  CHECK(StrView_EqCStr(win.Name, "ship"));

  PromptTemplate bare = {};
  CHECK(PromptTemplates_ParseFile(a, SVL("/tmp/prompts/bare.md"),
                                  SVL("\n\nFirst real line\nsecond\n"), &bare));
  CHECK_ALL(StrView_EqCStr(bare.Description, "First real line"),
            StrView_EqCStr(bare.Content, "First real line\nsecond"),
            bare.ArgumentHint.Length == 0);

  PromptTemplate cut = {};
  CHECK(PromptTemplates_ParseFile(
      a, SVL("/tmp/prompts/long.md"),
      SVL("012345678901234567890123456789012345678901234567890123456789EXTRA\n"),
      &cut));
  CHECK(StrView_EqCStr(
      cut.Description,
      "012345678901234567890123456789012345678901234567890123456789..."));

  PromptTemplate exact = {};
  CHECK(PromptTemplates_ParseFile(
      a, SVL("/tmp/prompts/exact.md"),
      SVL("012345678901234567890123456789012345678901234567890123456789\n"),
      &exact));
  CHECK(StrView_EqCStr(
      exact.Description,
      "012345678901234567890123456789012345678901234567890123456789"));

  PromptTemplate blank = {};
  CHECK(PromptTemplates_ParseFile(a, SVL("/tmp/prompts/blank.md"),
                                  SVL("---\n---\n"), &blank));
  CHECK_ALL(blank.Description.Length == 0, blank.Content.Length == 0);

  PromptTemplate crlf = {};
  CHECK(PromptTemplates_ParseFile(
      a, SVL("/tmp/prompts/crlf.md"),
      SVL("---\r\ndescription: Windows\r\n---\r\nBody line\r\n"), &crlf));
  CHECK_ALL(StrView_EqCStr(crlf.Description, "Windows"),
            StrView_EqCStr(crlf.Content, "Body line"));

  PromptTemplate unnamed = {};
  CHECK(!PromptTemplates_ParseFile(a, SVL(".md"), SVL("Body"), &unnamed));
}

bool WriteFile(Arena* a, StrView path, StrView content) {
  return ConfigFile_WriteAtomic(a, path, content);
}

u32 CountUnder(const PromptTemplateSet* set, const char* marker) {
  u32 count = 0;
  for (u32 i = 0; i < set->Templates.Count; i++)
    if (StrView_Contains(set->Templates.Data[i].FilePath, marker)) count++;
  return count;
}

void TestDiscovery(Arena* a) {
  const char* root = "clanker_prompt_template_test";
  StrView rootView = SV(root);
  StrView agentDir = Fs_Join(a, rootView, SVL("agent"));
  StrView globalDir = Fs_Join(a, agentDir, SVL("prompts"));
  StrView projectDir = Fs_Join(a, rootView, SVL("project"));
  StrView projectPrompts =
      Fs_Join(a, Fs_Join(a, projectDir, SVL(".clanker")), SVL("prompts"));

  CHECK(Fs_MkdirP(a, globalDir));
  CHECK(Fs_MkdirP(a, projectPrompts));
  CHECK(Fs_MkdirP(a, Fs_Join(a, globalDir, SVL("nested"))));

  CHECK(WriteFile(a, Fs_Join(a, globalDir, SVL("shared.md")),
                  SVL("---\ndescription: global one\n---\nglobal body")));
  CHECK(WriteFile(a, Fs_Join(a, globalDir, SVL("onlyglobal.md")),
                  SVL("global only")));
  CHECK(WriteFile(a, Fs_Join(a, globalDir, SVL("notes.txt")), SVL("ignored")));
  CHECK(WriteFile(a, Fs_Join(a, globalDir, SVL(".hidden.md")), SVL("ignored")));
  CHECK(WriteFile(a, Fs_Join(a, globalDir, SVL("nested/deep.md")),
                  SVL("ignored")));

  CHECK(WriteFile(a, Fs_Join(a, projectPrompts, SVL("shared.md")),
                  SVL("---\ndescription: project one\n---\nproject body")));
  CHECK(WriteFile(a, Fs_Join(a, projectPrompts, SVL("onlyproject.md")),
                  SVL("project only")));

  PromptTemplateLoadOptions options = {};
  options.AgentDirectory = agentDir;
  options.WorkingDirectory = projectDir;
  options.ProjectTrusted = true;

  PromptTemplateSet set = {};
  PromptTemplates_Load(a, &options, &set);
  CHECK(CountUnder(&set, root) == 3);
  CHECK_ALL(PromptTemplates_Find(&set, SVL("shared")) != nullptr,
            PromptTemplates_Find(&set, SVL("onlyglobal")) != nullptr,
            PromptTemplates_Find(&set, SVL("onlyproject")) != nullptr,
            PromptTemplates_Find(&set, SVL("deep")) == nullptr,
            PromptTemplates_Find(&set, SVL("notes")) == nullptr,
            PromptTemplates_Find(&set, SVL(".hidden")) == nullptr);
  const PromptTemplate* shared = PromptTemplates_Find(&set, SVL("shared"));
  CHECK_ALL(shared != nullptr, StrView_EqCStr(shared->Description, "global one"),
            StrView_EqCStr(shared->Content, "global body"));
  bool collision = false;
  for (u32 i = 0; i < set.Diagnostics.Count; i++)
    if (StrView_Contains(set.Diagnostics.Data[i].Message,
                         "is already defined by"))
      collision = true;
  CHECK(collision);

  options.ProjectTrusted = false;
  PromptTemplateSet untrusted = {};
  PromptTemplates_Load(a, &options, &untrusted);
  CHECK_ALL(CountUnder(&untrusted, root) == 2,
            PromptTemplates_Find(&untrusted, SVL("onlyproject")) == nullptr);

  options.ProjectTrusted = true;
  options.NoPromptTemplates = true;
  StrView explicitPath = Fs_Join(a, projectPrompts, SVL("onlyproject.md"));
  options.ExplicitPaths = &explicitPath;
  options.ExplicitPathCount = 1;
  PromptTemplateSet explicitOnly = {};
  PromptTemplates_Load(a, &options, &explicitOnly);
  CHECK_ALL(CountUnder(&explicitOnly, root) == 1,
            PromptTemplates_Find(&explicitOnly, SVL("onlyproject")) != nullptr,
            PromptTemplates_Find(&explicitOnly, SVL("shared")) == nullptr);

  options.NoPromptTemplates = false;
  PromptTemplateSet deduped = {};
  PromptTemplates_Load(a, &options, &deduped);
  CHECK(CountUnder(&deduped, root) == 3);

  StrView missing = Fs_Join(a, rootView, SVL("nope.md"));
  options.ExplicitPaths = &missing;
  PromptTemplateSet reported = {};
  PromptTemplates_Load(a, &options, &reported);
  bool sawMissing = false;
  for (u32 i = 0; i < reported.Diagnostics.Count; i++)
    if (StrView_Contains(reported.Diagnostics.Data[i].Message,
                         "does not exist"))
      sawMissing = true;
  CHECK(sawMissing);

  options.ExplicitPaths = &projectPrompts;
  options.NoPromptTemplates = true;
  PromptTemplateSet dir = {};
  PromptTemplates_Load(a, &options, &dir);
  CHECK(CountUnder(&dir, root) == 2);

  const PromptTemplate* tmpl = PromptTemplates_Find(&dir, SVL("onlyproject"));
  CHECK_ALL(tmpl != nullptr,
            StrView_EqCStr(PromptTemplates_Expand(a, tmpl, SVL("ignored")),
                           "project only"));
}

void Cleanup() {
  const char* files[] = {
      "clanker_prompt_template_test/agent/prompts/shared.md",
      "clanker_prompt_template_test/agent/prompts/onlyglobal.md",
      "clanker_prompt_template_test/agent/prompts/notes.txt",
      "clanker_prompt_template_test/agent/prompts/.hidden.md",
      "clanker_prompt_template_test/agent/prompts/nested/deep.md",
      "clanker_prompt_template_test/project/.clanker/prompts/shared.md",
      "clanker_prompt_template_test/project/.clanker/prompts/onlyproject.md",
  };
  for (u32 i = 0; i < sizeof(files) / sizeof(files[0]); i++) remove(files[i]);
  const char* dirs[] = {
      "clanker_prompt_template_test/agent/prompts/nested",
      "clanker_prompt_template_test/agent/prompts",
      "clanker_prompt_template_test/agent",
      "clanker_prompt_template_test/project/.clanker/prompts",
      "clanker_prompt_template_test/project/.clanker",
      "clanker_prompt_template_test/project",
      "clanker_prompt_template_test",
  };
  for (u32 i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) RemoveDir(dirs[i]);
}

}  // namespace

void TestPromptTemplates() {
  Arena a = ArenaCreate((usize)16 << 20);
  TestSplitArgs(&a);
  TestSubstitute(&a);
  TestParseFile(&a);
  TestDiscovery(&a);
  Cleanup();
  ArenaRelease(&a);
}
