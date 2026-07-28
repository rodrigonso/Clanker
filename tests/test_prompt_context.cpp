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

void RemoveDir(const char* path) {
#if defined(_WIN32)
  _rmdir(path);
#else
  rmdir(path);
#endif
}

void TestPromptAssembly(Arena* scratch, StrView root, StrView cwd,
                        StrView agentDirectory) {
  Tool tools[2] = {};
  tools[0].Name = SVL("read");
  tools[0].Description = SVL("Read a file");
  tools[1].Name = SVL("shell");
  tools[1].Description = SVL("Run a shell command");

  PromptContextOptions options = {};
  options.WorkingDirectory = cwd;
  options.AgentDirectory = agentDirectory;
  options.Tools = tools;
  options.ToolCount = 2;
  options.NoContextFiles = true;
  options.ProjectTrusted = true;
  PromptContext context = {};
  StrBuf error = StrBuf_Make(scratch, 128);
  CHECK(PromptContext_Build(&options, &context, &error));
  CHECK_ALL(StrView_Contains(context.SystemPrompt, "Available tools:"),
            StrView_Contains(context.SystemPrompt, "- read: Read a file"),
            StrView_Contains(context.SystemPrompt, "- shell: Run a shell command"),
            StrView_Contains(context.SystemPrompt, "Current working directory:"),
            !StrView_Contains(context.SystemPrompt, "2026"));
  PromptContext_Release(&context);

  StrView appendInputs[] = {SVL("first addition"), SVL("second addition")};
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
            context.ContextFiles.Count == 3);
  PromptContext_Release(&context);

  options.ProjectTrusted = true;
  StrBuf_Clear(&error);
  CHECK(PromptContext_Build(&options, &context, &error));
  CHECK_ALL(FindText(context.SystemPrompt, "project base") == 0,
            StrView_Contains(context.SystemPrompt, "project append"),
            StrView_Contains(context.SystemPrompt,
                             "<project_instructions path=\""),
            !StrView_Contains(context.SystemPrompt, "global base"),
            !StrView_Contains(context.SystemPrompt, "global append"));
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

  const char* files[] = {
      "clank_prompt_context_test/global/AGENTS.md",
      "clank_prompt_context_test/global/SYSTEM.md",
      "clank_prompt_context_test/global/APPEND_SYSTEM.md",
      "clank_prompt_context_test/AGENTS.md",
      "clank_prompt_context_test/work/child/CLAUDE.md",
      "clank_prompt_context_test/work/child/.clank/SYSTEM.md",
      "clank_prompt_context_test/work/child/.clank/APPEND_SYSTEM.md",
  };
  for (u32 i = 0; i < sizeof(files) / sizeof(files[0]); i++) remove(files[i]);
  RemoveDir("clank_prompt_context_test/work/child/AGENTS.md");
  RemoveDir("clank_prompt_context_test/work/child/.clank");
  RemoveDir("clank_prompt_context_test/work/child");
  RemoveDir("clank_prompt_context_test/work");
  RemoveDir("clank_prompt_context_test/global");
  RemoveDir("clank_prompt_context_test");
  ArenaRelease(&scratch);
}
