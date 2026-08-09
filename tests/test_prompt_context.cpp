#include <stdio.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "app/prompt_context.h"
#include "base/config_file.h"
#include "check.h"
#include "tools/fs.h"

namespace {

i32 FindText(StrView haystack, const char* needle) {
  StrView value = SV(needle);
  if (!value.Length || value.Length > haystack.Length) return -1;
  for (u32 i = 0; i + value.Length <= haystack.Length; i++) {
    if (memcmp(haystack.Data + i, value.Data, value.Length) == 0)
      return (i32)i;
  }
  return -1;
}

i32 CountText(StrView haystack, const char* needle) {
  StrView value = SV(needle);
  if (!value.Length || value.Length > haystack.Length) return 0;
  i32 count = 0;
  for (u32 i = 0; i + value.Length <= haystack.Length; i++) {
    if (memcmp(haystack.Data + i, value.Data, value.Length) == 0) count++;
  }
  return count;
}

void RemoveDir(const char* path) {
#if defined(_WIN32)
  _rmdir(path);
#else
  rmdir(path);
#endif
}

// Discovery walks every ancestor of cwd, so context files outside the test tree
// (the repository's own AGENTS.md, or one in the user's home) legitimately load
// too. Assertions count only what the test itself created.
u32 CountContextFilesUnder(const PromptContext* context, const char* marker) {
  u32 count = 0;
  for (u32 i = 0; i < context->ContextFiles.Count; i++)
    if (StrView_Contains(context->ContextFiles.Data[i].Path, marker)) count++;
  return count;
}

u32 CountSkillsUnder(const PromptContext* context, const char* marker) {
  u32 count = 0;
  for (u32 i = 0; i < context->Skills.Skills.Count; i++)
    if (StrView_Contains(context->Skills.Skills.Data[i].FilePath, marker))
      count++;
  return count;
}

constexpr StrView kReadGuidelines[] = {
    SVL("Use read to examine files instead of cat or sed."),
    SVL("Shared guideline")};
constexpr StrView kShellGuidelines[] = {SVL("Shared guideline")};

Tool MakeTool(const char* name, StrView snippet, const StrView* guidelines,
              u32 guidelineCount) {
  Tool tool = {};
  tool.Name = SV(name);
  tool.Description = SV("schema description, never used in the prompt");
  tool.PromptSnippet = snippet;
  tool.PromptGuidelines = guidelines;
  tool.PromptGuidelineCount = guidelineCount;
  return tool;
}
void TestPromptAssembly(Arena* scratch, StrView root, StrView cwd,
                        StrView agentDirectory) {
  Tool tools[3] = {
      MakeTool("read", SVL("Read file contents"), kReadGuidelines, 2),
      MakeTool("shell", SVL("Execute shell commands (ls, grep, find, etc.)"),
               kShellGuidelines, 1),
      // No snippet: must stay out of the "Available tools" list.
      MakeTool("silent_tool", {}, nullptr, 0)};

  PromptContextOptions options = {};
  options.WorkingDirectory = cwd;
  options.AgentDirectory = agentDirectory;
  options.Tools = tools;
  options.ToolCount = 3;
  options.NoContextFiles = true;
  options.ProjectTrusted = true;
  PromptContext context = {};
  StrBuf error = StrBuf_Make(scratch, 128);
  CHECK(PromptContext_Build(&options, &context, &error));
  // The whole default prompt, byte for byte, up to the varying cwd. Tool lines
  // follow tool order and use snippets; guidelines are the conditional shell
  // bullet, then tool contributions in tool order with duplicates collapsed,
  // then the two always-on bullets.
  CHECK_ALL(
      FindText(context.SystemPrompt,
               "You are an expert coding assistant operating inside Clank, a "
               "coding agent harness. You help users by reading files, "
               "executing commands, editing code, and writing new files."
               "\n\nAvailable tools:"
               "\n- read: Read file contents"
               "\n- shell: Execute shell commands (ls, grep, find, etc.)"
               "\n\nGuidelines:"
               "\n- Use shell for file operations like ls, rg, find"
               "\n- Use read to examine files instead of cat or sed."
               "\n- Shared guideline"
               "\n- Be concise in your responses"
               "\n- Show file paths clearly when working with files"
               "\nCurrent working directory: ") == 0,
      CountText(context.SystemPrompt, "- Shared guideline") == 1,
      !StrView_Contains(context.SystemPrompt, "silent_tool"),
      !StrView_Contains(context.SystemPrompt, "schema description"),
      !StrView_Contains(context.SystemPrompt, "2026"));
  PromptContext_Release(&context);

  // A dedicated search tool suppresses the shell fallback bullet.
  Tool searchTools[2] = {
      tools[1],
      MakeTool("grep", SVL("Search file contents for patterns"), nullptr, 0)};
  options.Tools = searchTools;
  options.ToolCount = 2;
  StrBuf_Clear(&error);
  CHECK(PromptContext_Build(&options, &context, &error));
  CHECK(!StrView_Contains(context.SystemPrompt, "Use shell for file operations"));
  PromptContext_Release(&context);

  options.Tools = nullptr;
  options.ToolCount = 0;
  StrBuf_Clear(&error);
  CHECK(PromptContext_Build(&options, &context, &error));
  CHECK(StrView_Contains(context.SystemPrompt, "Available tools:\n(none)\n"));
  PromptContext_Release(&context);

  StrView appendInputs[] = {SVL("first addition"), SVL("second addition")};
  options.Tools = tools;
  options.ToolCount = 3;
  options.HasExplicitSystemPrompt = true;
  options.ExplicitSystemPrompt = SVL("custom base");
  options.ExplicitAppendPrompts = appendInputs;
  options.ExplicitAppendPromptCount = 2;
  StrBuf_Clear(&error);
  CHECK(PromptContext_Build(&options, &context, &error));
  CHECK_ALL(FindText(context.SystemPrompt, "custom base") == 0,
            StrView_Contains(context.SystemPrompt,
                             "first addition\n\nsecond addition"),
            StrView_Contains(context.SystemPrompt, "Current working directory:"),
            !StrView_Contains(context.SystemPrompt, "Available tools:"));
  PromptContext_Release(&context);

  (void)root;
}

void TestDiscoveryAndTrust(Arena* scratch, StrView root, StrView cwd,
                           StrView agentDirectory) {
  StrView globalAgents = Fs_Join(scratch, agentDirectory, SVL("AGENTS.md"));
  StrView rootAgents = Fs_Join(scratch, root, SVL("AGENTS.md"));
  StrView agentsDirectory = Fs_Join(scratch, cwd, SVL("AGENTS.md"));
  StrView cwdClaude = Fs_Join(scratch, cwd, SVL("CLAUDE.md"));
  StrView globalSystem = Fs_Join(scratch, agentDirectory, SVL("SYSTEM.md"));
  StrView globalAppend =
      Fs_Join(scratch, agentDirectory, SVL("APPEND_SYSTEM.md"));
  StrView projectConfig = Fs_Join(scratch, cwd, SVL(".clank"));
  StrView projectSystem = Fs_Join(scratch, projectConfig, SVL("SYSTEM.md"));
  StrView projectAppend =
      Fs_Join(scratch, projectConfig, SVL("APPEND_SYSTEM.md"));
  CHECK_ALL(ConfigFile_WriteAtomic(scratch, globalAgents, SVL("global context")),
            ConfigFile_WriteAtomic(scratch, rootAgents, SVL("root context")),
            Fs_MkdirP(scratch, agentsDirectory),
            ConfigFile_WriteAtomic(scratch, cwdClaude, SVL("cwd context")),
            ConfigFile_WriteAtomic(scratch, globalSystem, SVL("global base")),
            ConfigFile_WriteAtomic(scratch, globalAppend, SVL("global append")),
            ConfigFile_WriteAtomic(scratch, projectSystem, SVL("project base")),
            ConfigFile_WriteAtomic(scratch, projectAppend, SVL("project append")));

  PromptContextOptions options = {};
  options.WorkingDirectory = cwd;
  options.AgentDirectory = agentDirectory;
  options.ProjectTrusted = false;
  PromptContext context = {};
  StrBuf error = StrBuf_Make(scratch, 128);
  CHECK(PromptContext_Build(&options, &context, &error));
  i32 globalAt = FindText(context.SystemPrompt, "global context");
  i32 rootAt = FindText(context.SystemPrompt, "root context");
  i32 cwdAt = FindText(context.SystemPrompt, "cwd context");
  CHECK_ALL(FindText(context.SystemPrompt, "global base") == 0,
            !StrView_Contains(context.SystemPrompt, "project base"),
            !StrView_Contains(context.SystemPrompt, "project append"),
            StrView_Contains(context.SystemPrompt, "global append"),
            globalAt >= 0 && rootAt > globalAt && cwdAt > rootAt,
            CountContextFilesUnder(&context, "clank_prompt_context_test") == 3);
  PromptContext_Release(&context);

  options.ProjectTrusted = true;
  StrBuf_Clear(&error);
  CHECK(PromptContext_Build(&options, &context, &error));
  CHECK_ALL(FindText(context.SystemPrompt, "project base") == 0,
            StrView_Contains(context.SystemPrompt, "project append"),
            StrView_Contains(context.SystemPrompt,
                             "<project_instructions path=\""),
            StrView_Contains(context.SystemPrompt,
                             "\n\n<project_context>\n\n"
                             "Project-specific instructions and guidelines:\n\n"
                             "<project_instructions path=\""),
            StrView_Contains(context.SystemPrompt,
                             "\n</project_instructions>\n\n"),
            StrView_Contains(context.SystemPrompt,
                             "</project_context>\n\nCurrent working directory:"),
            !StrView_Contains(context.SystemPrompt, "global base"),
            !StrView_Contains(context.SystemPrompt, "global append"));
  PromptContext_Release(&context);

  // AGENTS.override.md wins over other candidates in the same directory.
  StrView cwdOverride = Fs_Join(scratch, cwd, SVL("AGENTS.override.md"));
  CHECK(ConfigFile_WriteAtomic(scratch, cwdOverride, SVL("cwd override")));
  StrBuf_Clear(&error);
  CHECK(PromptContext_Build(&options, &context, &error));
  CHECK_ALL(StrView_Contains(context.SystemPrompt, "cwd override"),
            !StrView_Contains(context.SystemPrompt, "cwd context"),
            StrView_Contains(context.SystemPrompt, "root context"),
            CountContextFilesUnder(&context, "clank_prompt_context_test") == 3);
  PromptContext_Release(&context);
}

void TestSkillsBlock(Arena* scratch, StrView cwd, StrView agentDirectory) {
  StrView skillDir =
      Fs_Join(scratch, Fs_Join(scratch, agentDirectory, SVL("skills")),
              SVL("placement"));
  CHECK(Fs_MkdirP(scratch, skillDir));
  CHECK(ConfigFile_WriteAtomic(
      scratch, Fs_Join(scratch, skillDir, SVL("SKILL.md")),
      SVL("---\nname: placement\ndescription: Placement probe skill\n---\n"
          "Body text")));

  Tool readTool = MakeTool("read", SVL("Read file contents"), nullptr, 0);
  PromptContextOptions options = {};
  options.WorkingDirectory = cwd;
  options.AgentDirectory = agentDirectory;
  options.Tools = &readTool;
  options.ToolCount = 1;
  options.ProjectTrusted = true;
  PromptContext context = {};
  StrBuf error = StrBuf_Make(scratch, 128);
  CHECK(PromptContext_Build(&options, &context, &error));

  i32 projectEnd = FindText(context.SystemPrompt, "</project_context>\n");
  i32 blockAt = FindText(context.SystemPrompt, "\n\nThe following skills provide");
  i32 openAt = FindText(context.SystemPrompt, "<available_skills>");
  i32 closeAt = FindText(context.SystemPrompt, "</available_skills>");
  i32 cwdAt = FindText(context.SystemPrompt, "\nCurrent working directory: ");
  CHECK_ALL(projectEnd >= 0, blockAt > projectEnd, openAt > blockAt,
            closeAt > openAt, cwdAt > closeAt,
            StrView_Contains(
                context.SystemPrompt,
                "\n\nThe following skills provide specialized instructions for "
                "specific tasks.\nUse the read tool to load a skill's file when "
                "the task matches its description.\nWhen a skill file "
                "references a relative path, resolve it against the skill "
                "directory (parent of SKILL.md / dirname of the path) and use "
                "that absolute path in tool commands.\n\n<available_skills>\n"),
            StrView_Contains(context.SystemPrompt,
                             "  <skill>\n    <name>placement</name>\n"
                             "    <description>Placement probe skill"
                             "</description>\n    <location>"),
            StrView_Contains(context.SystemPrompt,
                             "</location>\n  </skill>\n</available_skills>"),
            CountSkillsUnder(&context, "clank_prompt_context_test") == 1);
  PromptContext_Release(&context);

  Tool shellTool = MakeTool("shell", SVL("Execute shell commands"), nullptr, 0);
  options.Tools = &shellTool;
  StrBuf_Clear(&error);
  CHECK(PromptContext_Build(&options, &context, &error));
  CHECK_ALL(!StrView_Contains(context.SystemPrompt, "<available_skills>"),
            CountSkillsUnder(&context, "clank_prompt_context_test") == 1);
  PromptContext_Release(&context);

  options.Tools = &readTool;
  options.NoSkills = true;
  StrBuf_Clear(&error);
  CHECK(PromptContext_Build(&options, &context, &error));
  CHECK_ALL(!StrView_Contains(context.SystemPrompt, "<available_skills>"),
            CountSkillsUnder(&context, "clank_prompt_context_test") == 0);
  PromptContext_Release(&context);
}
}  // namespace

void TestPromptContext() {
  Arena scratch = ArenaCreate((usize)64 << 20);
  StrView root = SVL("clank_prompt_context_test");
  StrView agentDirectory = Fs_Join(&scratch, root, SVL("global"));
  StrView work = Fs_Join(&scratch, root, SVL("work"));
  StrView cwd = Fs_Join(&scratch, work, SVL("child"));
  CHECK_ALL(Fs_MkdirP(&scratch, agentDirectory), Fs_MkdirP(&scratch, cwd));

  TestPromptAssembly(&scratch, root, cwd, agentDirectory);
  TestDiscoveryAndTrust(&scratch, root, cwd, agentDirectory);
  TestSkillsBlock(&scratch, cwd, agentDirectory);

  const char* files[] = {
      "clank_prompt_context_test/global/AGENTS.md",
      "clank_prompt_context_test/global/SYSTEM.md",
      "clank_prompt_context_test/global/APPEND_SYSTEM.md",
      "clank_prompt_context_test/AGENTS.md",
      "clank_prompt_context_test/work/child/CLAUDE.md",
      "clank_prompt_context_test/work/child/AGENTS.override.md",
      "clank_prompt_context_test/work/child/.clank/SYSTEM.md",
      "clank_prompt_context_test/work/child/.clank/APPEND_SYSTEM.md",
      "clank_prompt_context_test/global/skills/placement/SKILL.md",
  };
  for (u32 i = 0; i < sizeof(files) / sizeof(files[0]); i++) remove(files[i]);
  RemoveDir("clank_prompt_context_test/work/child/AGENTS.md");
  RemoveDir("clank_prompt_context_test/work/child/.clank");
  RemoveDir("clank_prompt_context_test/work/child");
  RemoveDir("clank_prompt_context_test/work");
  RemoveDir("clank_prompt_context_test/global/skills/placement");
  RemoveDir("clank_prompt_context_test/global/skills");
  RemoveDir("clank_prompt_context_test/global");
  RemoveDir("clank_prompt_context_test");
  ArenaRelease(&scratch);
}
