#include <stdio.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "app/skills.h"
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

bool WriteSkill(Arena* arena, StrView dir, const char* body) {
  return Fs_MkdirP(arena, dir) &&
         ConfigFile_WriteAtomic(arena, Fs_Join(arena, dir, SVL("SKILL.md")),
                                SV(body));
}

Skill ParseOne(Arena* arena, const char* path, const char* content,
               bool* ok, Array<LoadDiagnostic>* warnings) {
  Skill skill = {};
  *ok = Skills_ParseFile(arena, SV(path), SV(content), &skill, warnings);
  return skill;
}

void TestFrontmatter(Arena* scratch) {
  Array<LoadDiagnostic> warnings = Array_Make<LoadDiagnostic>(scratch, 8);
  bool ok = false;

  Skill basic = ParseOne(scratch, "/root/pdf-tools/SKILL.md",
                         "---\nname: pdf-tools\ndescription: Work with PDFs\n"
                         "---\nBody text here.\n",
                         &ok, &warnings);
  CHECK_ALL(ok, StrView_Eq(basic.Name, SVL("pdf-tools")),
            StrView_Eq(basic.Description, SVL("Work with PDFs")),
            StrView_Eq(basic.BaseDir, SVL("/root/pdf-tools")),
            !basic.DisableModelInvocation, warnings.Count == 0);

  Skill crlf = ParseOne(scratch, "/root/a/SKILL.md",
                        "---\r\nname: crlf-skill\r\ndescription: Handles CRLF\r\n"
                        "---\r\nBody\r\n",
                        &ok, &warnings);
  CHECK_ALL(ok, StrView_Eq(crlf.Name, SVL("crlf-skill")),
            StrView_Eq(crlf.Description, SVL("Handles CRLF")));

  Skill fallback = ParseOne(scratch, "/root/inferred-name/SKILL.md",
                            "---\ndescription: No name key\n---\nBody",
                            &ok, &warnings);
  CHECK_ALL(ok, StrView_Eq(fallback.Name, SVL("inferred-name")));

  Skill quoted = ParseOne(scratch, "/root/q/SKILL.md",
                          "---\nname: \"quoted-name\"\n"
                          "description: 'Single quoted'\n---\nBody",
                          &ok, &warnings);
  CHECK_ALL(ok, StrView_Eq(quoted.Name, SVL("quoted-name")),
            StrView_Eq(quoted.Description, SVL("Single quoted")));

  Skill folded = ParseOne(scratch, "/root/f/SKILL.md",
                          "---\nname: folded-name\ndescription: >\n"
                          "  first line\n  second line\n---\nBody",
                          &ok, &warnings);
  CHECK_ALL(ok, StrView_Eq(folded.Description, SVL("first line second line")));

  Skill literal = ParseOne(scratch, "/root/l/SKILL.md",
                           "---\nname: literal-name\ndescription: |\n"
                           "  first line\n  second line\n---\nBody",
                           &ok, &warnings);
  CHECK_ALL(ok, StrView_Eq(literal.Description, SVL("first line\nsecond line")));

  Skill extras = ParseOne(
      scratch, "/root/e/SKILL.md",
      "---\nname: extra-keys # inline comment\nallowed-tools:\n  - read\n"
      "  - shell\ncompatibility: pi\ndescription: Still parses\n---\nBody",
      &ok, &warnings);
  CHECK_ALL(ok, StrView_Eq(extras.Name, SVL("extra-keys")),
            StrView_Eq(extras.Description, SVL("Still parses")));

  Skill hidden = ParseOne(scratch, "/root/h/SKILL.md",
                          "---\nname: hidden-skill\ndescription: User only\n"
                          "disable-model-invocation: true\n---\nBody",
                          &ok, &warnings);
  CHECK_ALL(ok, hidden.DisableModelInvocation);

  Array<LoadDiagnostic> dropped = Array_Make<LoadDiagnostic>(scratch, 4);
  ParseOne(scratch, "/root/d/SKILL.md", "---\nname: no-description\n---\nBody",
           &ok, &dropped);
  CHECK_ALL(!ok, dropped.Count == 1);

  Array<LoadDiagnostic> unterminated = Array_Make<LoadDiagnostic>(scratch, 4);
  ParseOne(scratch, "/root/u/SKILL.md", "---\nname: x\ndescription: y\nBody",
           &ok, &unterminated);
  CHECK(!ok);
  CHECK(StrView_Eq(Skills_ExtractBody(scratch, SVL("# Plain\n\nNo frontmatter")),
                   SVL("# Plain\n\nNo frontmatter")));
  CHECK(StrView_Eq(
      Skills_ExtractBody(scratch,
                         SVL("---\nname: a\ndescription: b\n---\n\nBody\n")),
      SVL("Body")));

  Array<LoadDiagnostic> named = Array_Make<LoadDiagnostic>(scratch, 8);
  Skill bad = ParseOne(scratch, "/root/b/SKILL.md",
                       "---\nname: -Bad--Name-\ndescription: Warns\n---\nBody",
                       &ok, &named);
  CHECK_ALL(ok, StrView_Eq(bad.Name, SVL("-Bad--Name-")), named.Count == 3);
}

void TestPromptBlock(Arena* scratch) {
  Skill skills[3] = {};
  skills[0].Name = SVL("alpha");
  skills[0].Description = SVL("First skill");
  skills[0].FilePath = SVL("/skills/alpha/SKILL.md");
  skills[1].Name = SVL("beta");
  skills[1].Description = SVL("a & b < c > d \" e ' f");
  skills[1].FilePath = SVL("/skills/beta/SKILL.md");
  skills[2].Name = SVL("hidden");
  skills[2].Description = SVL("Never in the prompt");
  skills[2].FilePath = SVL("/skills/hidden/SKILL.md");
  skills[2].DisableModelInvocation = true;

  SkillSet set = {};
  set.Skills = Array_Make<Skill>(scratch, 4);
  for (u32 i = 0; i < 3; i++) Array_Push(&set.Skills, skills[i]);

  StrBuf out = StrBuf_Make(scratch, 512);
  CHECK(Skills_AppendPromptBlock(&out, &set, 1u << 20));
  CHECK(StrView_Eq(
      StrBuf_View(&out),
      SVL("\n\nThe following skills provide specialized instructions for "
          "specific tasks.\n"
          "Use the read tool to load a skill's file when the task matches its "
          "description.\n"
          "When a skill file references a relative path, resolve it against "
          "the skill directory (parent of SKILL.md / dirname of the path) and "
          "use that absolute path in tool commands.\n"
          "\n<available_skills>\n"
          "  <skill>\n"
          "    <name>alpha</name>\n"
          "    <description>First skill</description>\n"
          "    <location>/skills/alpha/SKILL.md</location>\n"
          "  </skill>\n"
          "  <skill>\n"
          "    <name>beta</name>\n"
          "    <description>a &amp; b &lt; c &gt; d &quot; e &apos; f"
          "</description>\n"
          "    <location>/skills/beta/SKILL.md</location>\n"
          "  </skill>\n"
          "</available_skills>")));

  SkillSet hiddenOnly = {};
  hiddenOnly.Skills = Array_Make<Skill>(scratch, 1);
  Array_Push(&hiddenOnly.Skills, skills[2]);
  StrBuf none = StrBuf_Make(scratch, 64);
  CHECK_ALL(Skills_AppendPromptBlock(&none, &hiddenOnly, 1u << 20),
            none.Length == 0);

  StrBuf capped = StrBuf_Make(scratch, 64);
  StrBuf_AppendCStr(&capped, "prefix");
  CHECK(!Skills_AppendPromptBlock(&capped, &set, 64));
  CHECK(StrView_Eq(StrBuf_View(&capped), SVL("prefix")));
}

void TestInvocation(Arena* scratch) {
  Skill skill = {};
  skill.Name = SVL("pdf-tools");
  skill.FilePath = SVL("/skills/pdf-tools/SKILL.md");
  skill.BaseDir = SVL("/skills/pdf-tools");

  CHECK(StrView_Eq(
      Skills_FormatInvocation(scratch, &skill, SVL("Do the thing."), {}),
      SVL("<skill name=\"pdf-tools\" location=\"/skills/pdf-tools/SKILL.md\">\n"
          "References are relative to /skills/pdf-tools.\n\n"
          "Do the thing.\n</skill>")));
  CHECK(StrView_Eq(
      Skills_FormatInvocation(scratch, &skill, SVL("Body."), SVL("  on a.pdf ")),
      SVL("<skill name=\"pdf-tools\" location=\"/skills/pdf-tools/SKILL.md\">\n"
          "References are relative to /skills/pdf-tools.\n\n"
          "Body.\n</skill>\n\non a.pdf")));
}

void TestDiscovery(Arena* scratch, StrView root) {
  StrView agentDir = Fs_Join(scratch, root, SVL("agent"));
  StrView userSkills = Fs_Join(scratch, agentDir, SVL("skills"));
  StrView work = Fs_Join(scratch, root, SVL("work"));
  StrView projectSkills =
      Fs_Join(scratch, Fs_Join(scratch, work, SVL(".clanker")), SVL("skills"));

  StrView outer = Fs_Join(scratch, userSkills, SVL("outer"));
  CHECK(WriteSkill(scratch, outer,
                   "---\nname: outer\ndescription: Outer skill\n---\nOuter body"));
  CHECK(WriteSkill(scratch, Fs_Join(scratch, outer, SVL("nested")),
                   "---\nname: nested\ndescription: Should not load\n---\nx"));

  CHECK(ConfigFile_WriteAtomic(
      scratch, Fs_Join(scratch, userSkills, SVL("loose.md")),
      SVL("---\nname: loose\ndescription: Root level markdown\n---\nLoose")));
  StrView deep = Fs_Join(scratch, userSkills, SVL("deep"));
  CHECK(Fs_MkdirP(scratch, deep));
  CHECK(ConfigFile_WriteAtomic(
      scratch, Fs_Join(scratch, deep, SVL("buried.md")),
      SVL("---\nname: buried\ndescription: Must not load\n---\nx")));

  CHECK(WriteSkill(scratch, Fs_Join(scratch, userSkills, SVL(".hidden")),
                   "---\nname: dotted\ndescription: Skipped\n---\nx"));
  CHECK(WriteSkill(scratch, Fs_Join(scratch, userSkills, SVL("node_modules")),
                   "---\nname: vendored\ndescription: Skipped\n---\nx"));

  CHECK(ConfigFile_WriteAtomic(
      scratch, Fs_Join(scratch, userSkills, SVL(".fdignore")), SVL("ignored\n")));
  CHECK(WriteSkill(scratch, Fs_Join(scratch, userSkills, SVL("ignored")),
                   "---\nname: ignored\ndescription: Skipped\n---\nx"));

  CHECK(WriteSkill(scratch, Fs_Join(scratch, projectSkills, SVL("outer")),
                   "---\nname: outer\ndescription: Project duplicate\n---\nx"));
  CHECK(WriteSkill(scratch, Fs_Join(scratch, projectSkills, SVL("only-proj")),
                   "---\nname: only-proj\ndescription: Project skill\n---\nx"));

  SkillLoadOptions options = {};
  options.AgentDirectory = agentDir;
  options.WorkingDirectory = work;
  options.ProjectTrusted = true;
  SkillSet loaded = {};
  Skills_Load(scratch, &options, &loaded);

  CHECK_ALL(Skills_Find(&loaded, SVL("outer")) != nullptr,
            Skills_Find(&loaded, SVL("loose")) != nullptr,
            Skills_Find(&loaded, SVL("only-proj")) != nullptr,
            Skills_Find(&loaded, SVL("nested")) == nullptr,
            Skills_Find(&loaded, SVL("buried")) == nullptr,
            Skills_Find(&loaded, SVL("dotted")) == nullptr,
            Skills_Find(&loaded, SVL("vendored")) == nullptr,
            Skills_Find(&loaded, SVL("ignored")) == nullptr,
            loaded.Skills.Count == 3);
  const Skill* outerSkill = Skills_Find(&loaded, SVL("outer"));
  CHECK_ALL(StrView_Eq(outerSkill->Description, SVL("Outer skill")),
            StrView_Contains(outerSkill->FilePath, "SKILL.md"),
            !StrView_Contains(outerSkill->FilePath, "\\"));
  bool sawCollision = false;
  for (u32 i = 0; i < loaded.Diagnostics.Count; i++)
    if (StrView_Contains(loaded.Diagnostics.Data[i].Message, "already defined"))
      sawCollision = true;
  CHECK(sawCollision);

  options.ProjectTrusted = false;
  SkillSet untrusted = {};
  Skills_Load(scratch, &options, &untrusted);
  CHECK_ALL(Skills_Find(&untrusted, SVL("only-proj")) == nullptr,
            Skills_Find(&untrusted, SVL("outer")) != nullptr);

  StrView explicitPath = Fs_Join(scratch, projectSkills, SVL("only-proj"));
  options.NoSkills = true;
  options.ExplicitPaths = &explicitPath;
  options.ExplicitPathCount = 1;
  SkillSet explicitOnly = {};
  Skills_Load(scratch, &options, &explicitOnly);
  CHECK_ALL(explicitOnly.Skills.Count == 1,
            Skills_Find(&explicitOnly, SVL("only-proj")) != nullptr);

  StrView missing = Fs_Join(scratch, root, SVL("does-not-exist"));
  options.ExplicitPaths = &missing;
  SkillSet absent = {};
  Skills_Load(scratch, &options, &absent);
  CHECK_ALL(absent.Skills.Count == 0, absent.Diagnostics.Count == 1);
}

void TestFileLoading(Arena* a, StrView root) {
  StrView dir = Fs_Join(a, root, SVL("files"));
  CHECK(Fs_MkdirP(a, dir));
  StrView first = Fs_Join(a, dir, SVL("first.txt"));
  StrView invalid = Fs_Join(a, dir, SVL("invalid.md"));
  StrView large = Fs_Join(a, dir, SVL("large.md"));
  StrView conflict = Fs_Join(a, dir, SVL("conflict.md"));
  StrView limit = Fs_Join(a, dir, SVL("limit.md"));
  StrView missing = Fs_Join(a, dir, SVL("missing.md"));
  CHECK(ConfigFile_WriteAtomic(
      a, first, SVL("---\nname: file-first\ndescription: First file\n---\nBody")));
  CHECK(ConfigFile_WriteAtomic(a, invalid, SVL("Missing description")));
  CHECK(ConfigFile_WriteAtomic(
      a, conflict,
      SVL("---\nname: file-first\ndescription: Losing file\n---\nBody")));

  const u32 maxBytes = 1u << 20;
  char* content = ArenaPushArray(a, char, maxBytes + 1);
  memset(content, 'x', maxBytes + 1);
  StrView header = SVL("---\nname: file-limit\ndescription: Limit file\n---\n");
  memcpy(content, header.Data, header.Length);
  CHECK(ConfigFile_WriteAtomic(a, large, StrView{content, maxBytes + 1}));
  CHECK(ConfigFile_WriteAtomic(a, limit, StrView{content, maxBytes}));

#if !defined(_WIN32)
  StrView alias = Fs_Join(a, dir, SVL("alias.md"));
  CHECK(symlink("first.txt", StrView_DupCStr(a, alias)) == 0);
#endif
  StrView paths[] = {
      first, Fs_Join(a, dir, SVL("./first.txt")),
      invalid, Fs_Join(a, dir, SVL("./invalid.md")),
      large, Fs_Join(a, dir, SVL("./large.md")),
      conflict, conflict, limit, missing, missing,
#if !defined(_WIN32)
      alias,
#endif
  };
  SkillLoadOptions options = {};
  options.NoSkills = true;
  options.ExplicitPaths = paths;
  options.ExplicitPathCount = (u32)(sizeof(paths) / sizeof(paths[0]));
  SkillSet set = {};
  Skills_Load(a, &options, &set);
  CHECK_ALL(set.Skills.Count == 2, set.Diagnostics.Count == 5);
  const Skill* firstSkill = Skills_Find(&set, SVL("file-first"));
  const Skill* limitSkill = Skills_Find(&set, SVL("file-limit"));
  CHECK_ALL(firstSkill != nullptr, limitSkill != nullptr);
  if (firstSkill && limitSkill) {
    CHECK_ALL(firstSkill == &set.Skills.Data[0],
              limitSkill == &set.Skills.Data[1],
              StrView_EqCStr(firstSkill->Description, "First file"),
              StrView_EqCStr(limitSkill->Description, "Limit file"),
              Fs_PathEquals(firstSkill->FilePath, Fs_RealPath(a, first)),
              Fs_PathEquals(firstSkill->BaseDir, Fs_RealPath(a, dir)));
    if (set.Diagnostics.Count == 5) {
      StrBuf collision = StrBuf_Make(a, 128 + firstSkill->FilePath.Length);
      StrBuf_AppendCStr(&collision,
                        "skill name 'file-first' is already defined by ");
      StrBuf_Append(&collision, firstSkill->FilePath);
      CHECK_ALL(
          Fs_PathEquals(set.Diagnostics.Data[0].Path, Fs_RealPath(a, invalid)),
          StrView_EqCStr(set.Diagnostics.Data[0].Message,
                         "skill is missing a frontmatter description"),
          Fs_PathEquals(set.Diagnostics.Data[1].Path, Fs_RealPath(a, large)),
          StrView_EqCStr(set.Diagnostics.Data[1].Message,
                         "skill file exceeds the 1 MiB limit"),
          Fs_PathEquals(set.Diagnostics.Data[2].Path, Fs_RealPath(a, conflict)),
          StrView_Eq(set.Diagnostics.Data[2].Message, StrBuf_View(&collision)));
      for (u32 i = 3; i < 5; i++)
        CHECK_ALL(StrView_Eq(set.Diagnostics.Data[i].Path, missing),
                  StrView_EqCStr(set.Diagnostics.Data[i].Message,
                                 "skill path does not exist"));
    }
  }
  const StrView files[] = {first, invalid, large, conflict, limit};
  for (u32 i = 0; i < sizeof(files) / sizeof(files[0]); i++)
    remove(StrView_DupCStr(a, files[i]));
#if !defined(_WIN32)
  remove(StrView_DupCStr(a, alias));
#endif
  RemoveDir(StrView_DupCStr(a, dir));
}

#if !defined(_WIN32)
void TestSymlinkCycle(Arena* a, StrView root) {
  StrView dir = Fs_Join(a, root, SVL("links"));
  StrView real = Fs_Join(a, dir, SVL("real"));
  CHECK(WriteSkill(a, real, "---\nname: linked\ndescription: Linked skill\n---\n"));
  StrView alias = Fs_Join(a, dir, SVL("alias"));
  StrView loop = Fs_Join(a, dir, SVL("loop"));
  CHECK(symlink("real", StrView_DupCStr(a, alias)) == 0);
  CHECK(symlink(".", StrView_DupCStr(a, loop)) == 0);
  SkillLoadOptions options = {};
  options.NoSkills = true;
  options.ExplicitPaths = &dir;
  options.ExplicitPathCount = 1;
  SkillSet set = {};
  Skills_Load(a, &options, &set);
  CHECK_ALL(set.Skills.Count == 1, set.Diagnostics.Count == 0,
            Skills_Find(&set, SVL("linked")) != nullptr);
  remove(StrView_DupCStr(a, alias));
  remove(StrView_DupCStr(a, loop));
  remove(StrView_DupCStr(a, Fs_Join(a, real, SVL("SKILL.md"))));
  RemoveDir(StrView_DupCStr(a, real));
  RemoveDir(StrView_DupCStr(a, dir));
}
#endif

void Cleanup(const char* root) {
  const char* files[] = {
      "clanker_skills_test/agent/skills/outer/SKILL.md",
      "clanker_skills_test/agent/skills/outer/nested/SKILL.md",
      "clanker_skills_test/agent/skills/loose.md",
      "clanker_skills_test/agent/skills/deep/buried.md",
      "clanker_skills_test/agent/skills/.hidden/SKILL.md",
      "clanker_skills_test/agent/skills/node_modules/SKILL.md",
      "clanker_skills_test/agent/skills/ignored/SKILL.md",
      "clanker_skills_test/agent/skills/.fdignore",
      "clanker_skills_test/work/.clanker/skills/outer/SKILL.md",
      "clanker_skills_test/work/.clanker/skills/only-proj/SKILL.md",
  };
  for (u32 i = 0; i < sizeof(files) / sizeof(files[0]); i++) remove(files[i]);
  const char* dirs[] = {
      "clanker_skills_test/agent/skills/outer/nested",
      "clanker_skills_test/agent/skills/outer",
      "clanker_skills_test/agent/skills/deep",
      "clanker_skills_test/agent/skills/.hidden",
      "clanker_skills_test/agent/skills/node_modules",
      "clanker_skills_test/agent/skills/ignored",
      "clanker_skills_test/agent/skills",
      "clanker_skills_test/agent",
      "clanker_skills_test/work/.clanker/skills/outer",
      "clanker_skills_test/work/.clanker/skills/only-proj",
      "clanker_skills_test/work/.clanker/skills",
      "clanker_skills_test/work/.clanker",
      "clanker_skills_test/work",
  };
  for (u32 i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) RemoveDir(dirs[i]);
  RemoveDir(root);
}

}  // namespace

void TestSkills() {
  Arena scratch = ArenaCreate((usize)64 << 20);
  StrView root = SVL("clanker_skills_test");
  CHECK(Fs_MkdirP(&scratch, root));

  TestFrontmatter(&scratch);
  TestPromptBlock(&scratch);
  TestInvocation(&scratch);
  TestDiscovery(&scratch, root);
  TestFileLoading(&scratch, root);
#if !defined(_WIN32)
  TestSymlinkCycle(&scratch, root);
#endif

  Cleanup("clanker_skills_test");
  ArenaRelease(&scratch);
}
