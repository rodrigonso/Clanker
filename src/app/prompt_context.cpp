#include "app/prompt_context.h"

#include "app/skills.h"
#include "base/config_file.h"
#include "tools/fs.h"

namespace {

constexpr u64 kMaxPromptFileBytes = (u64)4 << 20;
constexpr u32 kMaxSystemPromptBytes = (u32)16 << 20;

const StrView kContextCandidates[] = {
    SVL("AGENTS.override.md"), SVL("AGENTS.md"), SVL("AGENTS.MD"),
    SVL("CLAUDE.md"), SVL("CLAUDE.MD")};

// Identity text for the built-in prompt, mirroring pi's `buildSystemPrompt`.
constexpr const char* kDefaultPromptPreamble =
    "You are an expert coding assistant operating inside Clank, a coding agent "
    "harness. You help users by reading files, executing commands, editing "
    "code, and writing new files.";

StrView PathRoot(StrView path) {
  if (!path.Length) return {};
#if defined(_WIN32)
  if (path.Length >= 3 && path.Data[1] == ':' &&
      Fs_IsSep(path.Data[2]))
    return StrView{path.Data, 3};
  if (path.Length >= 2 && Fs_IsSep(path.Data[0]) &&
      Fs_IsSep(path.Data[1])) {
    u32 separators = 0;
    for (u32 i = 2; i < path.Length; i++) {
      if (!Fs_IsSep(path.Data[i])) continue;
      separators++;
      while (i + 1 < path.Length && Fs_IsSep(path.Data[i + 1])) i++;
      if (separators == 2) return StrView{path.Data, i + 1};
    }
    return path;
  }
#endif
  return Fs_IsSep(path.Data[0]) ? StrView{path.Data, 1} : StrView{};
}

bool AppendLimited(StrBuf* out, StrView value) {
  if ((u64)out->Length + value.Length > kMaxSystemPromptBytes) return false;
  StrBuf_Append(out, value);
  return true;
}

bool AppendCStrLimited(StrBuf* out, const char* value) {
  return AppendLimited(out, SV(value));
}

void AddDiagnostic(PromptContext* context, PromptDiagnosticSeverity severity,
                   StrView path, StrView message) {
  PromptDiagnostic diagnostic = {};
  diagnostic.Severity = severity;
  diagnostic.Path = StrView_Dup(&context->Mem, path);
  diagnostic.Message = StrView_Dup(&context->Mem, message);
  Array_Push(&context->Diagnostics, diagnostic);
}

enum class PromptFileReadResult : u8 {
  Missing,
  Directory,
  Success,
  TooLarge,
  Failure,
};

PromptFileReadResult ReadPromptFile(PromptContext* context, StrView path,
                                    StrView* content) {
  *content = {};
  FsStat stat = Fs_Stat(path);
  if (!stat.Exists) return PromptFileReadResult::Missing;
  if (stat.IsDir) return PromptFileReadResult::Directory;
  if (stat.Size > kMaxPromptFileBytes) return PromptFileReadResult::TooLarge;
  ConfigFileReadResult read = ConfigFile_ReadAll(&context->Mem, path, content);
  if (read == ConfigFileReadResult::Success)
    return PromptFileReadResult::Success;
  if (read == ConfigFileReadResult::Missing)
    return PromptFileReadResult::Missing;
  return PromptFileReadResult::Failure;
}

bool ContextAlreadyLoaded(const PromptContext* context, StrView path) {
  for (u32 i = 0; i < context->ContextFiles.Count; i++) {
    if (Fs_PathEquals(context->ContextFiles.Data[i].Path, path)) return true;
  }
  return false;
}

void DiscoverContextInDirectory(PromptContext* context, StrView directory) {
  u64 loadedBytes = 0;
  for (u32 i = 0; i < context->ContextFiles.Count; i++)
    loadedBytes += context->ContextFiles.Data[i].Content.Length;

  for (u32 i = 0; i < sizeof(kContextCandidates) / sizeof(kContextCandidates[0]);
       i++) {
    StrView path = Fs_Join(&context->Mem, directory, kContextCandidates[i]);
    FsStat stat = Fs_Stat(path);
    if (!stat.Exists || stat.IsDir) continue;

    StrView canonical = Fs_RealPath(&context->Mem, path);
    if (ContextAlreadyLoaded(context, canonical)) return;

    if (loadedBytes + stat.Size > kMaxSystemPromptBytes) {
      AddDiagnostic(context, PromptDiagnosticSeverity::Warning, path,
                    SVL("combined context files exceed the 16 MiB prompt limit"));
      return;
    }

    StrView content = {};
    PromptFileReadResult read = ReadPromptFile(context, path, &content);
    if (read == PromptFileReadResult::Success) {
      PromptContextFile file = {};
      file.Path = canonical;
      file.Content = content;
      Array_Push(&context->ContextFiles, file);
      return;
    }
    if (read == PromptFileReadResult::TooLarge) {
      AddDiagnostic(context, PromptDiagnosticSeverity::Warning, path,
                    SVL("context file exceeds the 4 MiB limit"));
    } else if (read == PromptFileReadResult::Failure) {
      AddDiagnostic(context, PromptDiagnosticSeverity::Warning, path,
                    SVL("context file could not be read"));
    }
  }
}

void DiscoverContextFiles(PromptContext* context,
                          const PromptContextOptions* options) {
  if (options->NoContextFiles) return;
  if (options->AgentDirectory.Length)
    DiscoverContextInDirectory(context, options->AgentDirectory);

  StrView cwd = Fs_RealPath(&context->Mem, options->WorkingDirectory);
  Array<StrView> ancestors = Array_Make<StrView>(&context->Mem, 16);
  StrView current = cwd;
  while (current.Length) {
    Array_Push(&ancestors, current);
    StrView parent = Fs_Parent(current);
    if (!parent.Length) {
      StrView root = PathRoot(current);
      if (root.Length && !Fs_PathEquals(root, current))
        Array_Push(&ancestors, StrView_Dup(&context->Mem, root));
      break;
    }
    current = StrView_Dup(&context->Mem, parent);
  }

  while (ancestors.Count) {
    StrView directory = Array_Pop(&ancestors);
    DiscoverContextInDirectory(context, directory);
  }
}

void DiscoverSkills(PromptContext* context,
                    const PromptContextOptions* options) {
  SkillLoadOptions skillOptions = {};
  skillOptions.AgentDirectory = options->AgentDirectory;
  skillOptions.WorkingDirectory = options->WorkingDirectory;
  skillOptions.ProjectTrusted = options->ProjectTrusted;
  skillOptions.NoSkills = options->NoSkills;
  skillOptions.SettingsPaths = context->SettingsSkillPaths.Data;
  skillOptions.SettingsPathCount = context->SettingsSkillPaths.Count;
  skillOptions.ExplicitPaths = options->SkillPaths;
  skillOptions.ExplicitPathCount = options->SkillPathCount;
  Skills_Load(&context->Mem, &skillOptions, &context->Skills);
  for (u32 i = 0; i < context->Skills.Diagnostics.Count; i++) {
    const SkillDiagnostic& diagnostic = context->Skills.Diagnostics.Data[i];
    Array_Push(&context->Diagnostics,
               PromptDiagnostic{PromptDiagnosticSeverity::Warning,
                                diagnostic.Path, diagnostic.Message});
  }
}

void DiscoverPromptTemplates(PromptContext* context,
                             const PromptContextOptions* options) {
  PromptTemplateLoadOptions templateOptions = {};
  templateOptions.AgentDirectory = options->AgentDirectory;
  templateOptions.WorkingDirectory = options->WorkingDirectory;
  templateOptions.ProjectTrusted = options->ProjectTrusted;
  templateOptions.NoPromptTemplates = options->NoPromptTemplates;
  templateOptions.SettingsPaths = context->SettingsPromptTemplatePaths.Data;
  templateOptions.SettingsPathCount = context->SettingsPromptTemplatePaths.Count;
  templateOptions.ExplicitPaths = options->PromptTemplatePaths;
  templateOptions.ExplicitPathCount = options->PromptTemplatePathCount;
  PromptTemplates_Load(&context->Mem, &templateOptions, &context->Templates);
  for (u32 i = 0; i < context->Templates.Diagnostics.Count; i++) {
    const PromptTemplateDiagnostic& diagnostic =
        context->Templates.Diagnostics.Data[i];
    Array_Push(&context->Diagnostics,
               PromptDiagnostic{PromptDiagnosticSeverity::Warning,
                                diagnostic.Path, diagnostic.Message});
  }
}

bool ReadRequiredPromptFile(PromptContext* context, StrView path,
                            StrView label, StrView* content, StrBuf* error) {
  PromptFileReadResult read = ReadPromptFile(context, path, content);
  if (read == PromptFileReadResult::Success) return true;
  StrBuf_AppendCStr(error, "failed to read ");
  StrBuf_Append(error, label);
  StrBuf_AppendCStr(error, " file ");
  StrBuf_Append(error, path);
  if (read == PromptFileReadResult::TooLarge)
    StrBuf_AppendCStr(error, ": file exceeds the 4 MiB limit");
  return false;
}

bool ResolveExplicitInput(PromptContext* context, StrView input, StrView label,
                          StrView* content, StrBuf* error) {
  FsStat stat = Fs_Stat(input);
  if (!stat.Exists || stat.IsDir) {
    *content = StrView_Dup(&context->Mem, input);
    return true;
  }
  return ReadRequiredPromptFile(context, input, label, content, error);
}

bool ResolveConfiguredPrompt(PromptContext* context,
                             const PromptContextOptions* options,
                             StrView fileName, StrView projectLabel,
                             StrView globalLabel, StrView* source,
                             StrView* content, StrBuf* error) {
  if (options->ProjectTrusted) {
    StrView projectConfig =
        Fs_Join(&context->Mem, options->WorkingDirectory, SVL(".clank"));
    StrView projectPath = Fs_Join(&context->Mem, projectConfig, fileName);
    FsStat stat = Fs_Stat(projectPath);
    if (stat.Exists && !stat.IsDir) {
      *source = Fs_RealPath(&context->Mem, projectPath);
      return ReadRequiredPromptFile(context, projectPath, projectLabel, content,
                                    error);
    }
  }

  StrView globalPath =
      Fs_Join(&context->Mem, options->AgentDirectory, fileName);
  FsStat stat = Fs_Stat(globalPath);
  if (!stat.Exists || stat.IsDir) return true;
  *source = Fs_RealPath(&context->Mem, globalPath);
  return ReadRequiredPromptFile(context, globalPath, globalLabel, content,
                                error);
}

bool ResolveSystemPrompt(PromptContext* context,
                         const PromptContextOptions* options,
                         StrView* basePrompt, StrBuf* error) {
  if (options->HasExplicitSystemPrompt) {
    context->SystemPromptSource = SVL("<command line>");
    return ResolveExplicitInput(context, options->ExplicitSystemPrompt,
                                SVL("system prompt"), basePrompt, error);
  }

  return ResolveConfiguredPrompt(
      context, options, SVL("SYSTEM.md"), SVL("project system prompt"),
      SVL("global system prompt"), &context->SystemPromptSource, basePrompt,
      error);
}

bool ResolveAppendPrompt(PromptContext* context,
                         const PromptContextOptions* options,
                         StrView* appendPrompt, StrBuf* error) {
  if (options->ExplicitAppendPromptCount) {
    context->AppendPromptSource = SVL("<command line>");
    StrBuf combined = StrBuf_Make(&context->Mem, 256);
    for (u32 i = 0; i < options->ExplicitAppendPromptCount; i++) {
      StrView value = {};
      if (!ResolveExplicitInput(context, options->ExplicitAppendPrompts[i],
                                SVL("append system prompt"), &value, error))
        return false;
      if (i && !AppendCStrLimited(&combined, "\n\n")) {
        StrBuf_AppendCStr(error, "append system prompt exceeds the 16 MiB limit");
        return false;
      }
      if (!AppendLimited(&combined, value)) {
        StrBuf_AppendCStr(error, "append system prompt exceeds the 16 MiB limit");
        return false;
      }
    }
    *appendPrompt = StrBuf_View(&combined);
    return true;
  }

  return ResolveConfiguredPrompt(
      context, options, SVL("APPEND_SYSTEM.md"),
      SVL("project append system prompt"), SVL("global append system prompt"),
      &context->AppendPromptSource, appendPrompt, error);
}

bool AppendEscapedAttribute(StrBuf* out, StrView value) {
  for (u32 i = 0; i < value.Length; i++) {
    StrView escaped = {};
    switch (value.Data[i]) {
      case '&': escaped = SVL("&amp;"); break;
      case '<': escaped = SVL("&lt;"); break;
      case '>': escaped = SVL("&gt;"); break;
      case '"': escaped = SVL("&quot;"); break;
      default:
        if (!AppendLimited(out, StrView{value.Data + i, 1})) return false;
        continue;
    }
    if (!AppendLimited(out, escaped)) return false;
  }
  return true;
}

bool HasTool(const PromptContextOptions* options, StrView name) {
  for (u32 i = 0; i < options->ToolCount; i++)
    if (StrView_Eq(options->Tools[i].Name, name)) return true;
  return false;
}

void AddGuideline(Array<StrView>* guidelines, StrView value) {
  if (!value.Length) return;
  for (u32 i = 0; i < guidelines->Count; i++)
    if (StrView_Eq(guidelines->Data[i], value)) return;
  Array_Push(guidelines, value);
}

// A tool is listed only when it supplies a snippet, so tools that exist purely
// for the model's benefit stay out of the prompt.
bool AppendToolList(const PromptContextOptions* options, StrBuf* prompt) {
  bool listedAny = false;
  for (u32 i = 0; i < options->ToolCount; i++) {
    const Tool& tool = options->Tools[i];
    if (!tool.PromptSnippet.Length) continue;
    if (!AppendCStrLimited(prompt, "- ") || !AppendLimited(prompt, tool.Name) ||
        !AppendCStrLimited(prompt, ": ") ||
        !AppendLimited(prompt, tool.PromptSnippet) ||
        !AppendCStrLimited(prompt, "\n"))
      return false;
    listedAny = true;
  }
  return listedAny || AppendCStrLimited(prompt, "(none)\n");
}

// Guideline order mirrors pi: the shell fallback bullet, then each tool's own
// bullets in tool order, then the always-on pair. Duplicates are dropped so
// tools can share advice without repeating it in the prompt.
Array<StrView> CollectGuidelines(Arena* arena,
                                 const PromptContextOptions* options) {
  Array<StrView> guidelines = Array_Make<StrView>(arena, 16);
  // Only worth steering the model toward the shell when no dedicated search or
  // listing tool is active. Keyed on Clank's built-in tool names.
  if (HasTool(options, SVL("shell")) && !HasTool(options, SVL("grep")) &&
      !HasTool(options, SVL("find")) && !HasTool(options, SVL("ls")))
    AddGuideline(&guidelines,
                 SVL("Use shell for file operations like ls, rg, find"));

  for (u32 i = 0; i < options->ToolCount; i++) {
    const Tool& tool = options->Tools[i];
    for (u32 j = 0; j < tool.PromptGuidelineCount; j++)
      AddGuideline(&guidelines, tool.PromptGuidelines[j]);
  }

  AddGuideline(&guidelines, SVL("Be concise in your responses"));
  AddGuideline(&guidelines,
               SVL("Show file paths clearly when working with files"));
  return guidelines;
}

// Mirrors pi's `buildSystemPrompt` default branch: a fixed preamble, an
// opt-in tool list built from one-line snippets, then guidelines composed from
// the active tool set.
bool BuildDefaultPrompt(PromptContext* context,
                        const PromptContextOptions* options, StrBuf* prompt) {
  if (!AppendCStrLimited(prompt, kDefaultPromptPreamble) ||
      !AppendCStrLimited(prompt, "\n\nAvailable tools:\n") ||
      !AppendToolList(options, prompt) ||
      !AppendCStrLimited(prompt, "\nGuidelines:"))
    return false;

  Array<StrView> guidelines = CollectGuidelines(&context->Mem, options);
  for (u32 i = 0; i < guidelines.Count; i++) {
    if (!AppendCStrLimited(prompt, "\n- ") ||
        !AppendLimited(prompt, guidelines.Data[i]))
      return false;
  }
  return true;
}

bool BuildFinalPrompt(PromptContext* context,
                      const PromptContextOptions* options,
                      StrView basePrompt, StrView appendPrompt,
                      StrBuf* error) {
  StrBuf prompt = StrBuf_Make(&context->Mem, 4096);
  if (basePrompt.Data) {
    if (!AppendLimited(&prompt, basePrompt)) goto too_large;
  } else if (!BuildDefaultPrompt(context, options, &prompt)) {
    goto too_large;
  }

  if (appendPrompt.Data) {
    if (!AppendCStrLimited(&prompt, "\n\n") ||
        !AppendLimited(&prompt, appendPrompt))
      goto too_large;
  }

  if (context->ContextFiles.Count) {
    if (!AppendCStrLimited(
            &prompt,
            "\n\n<project_context>\n\n"
            "Project-specific instructions and guidelines:\n\n"))
      goto too_large;
    for (u32 i = 0; i < context->ContextFiles.Count; i++) {
      const PromptContextFile& file = context->ContextFiles.Data[i];
      StrView posixPath = Fs_ToPosix(&context->Mem, file.Path);
      if (!AppendCStrLimited(&prompt, "<project_instructions path=\"") ||
          !AppendEscapedAttribute(&prompt, posixPath) ||
          !AppendCStrLimited(&prompt, "\">\n") ||
          !AppendLimited(&prompt, file.Content) ||
          !AppendCStrLimited(&prompt, "\n</project_instructions>\n\n"))
        goto too_large;
    }
    if (!AppendCStrLimited(&prompt, "</project_context>\n")) goto too_large;
  }

  // pi exposes skills only when the model can read their files.
  if (HasTool(options, SVL("read"))) {
    if (!Skills_AppendPromptBlock(&prompt, &context->Skills,
                                  kMaxSystemPromptBytes))
      goto too_large;
  }

  {
    StrView cwd = Fs_ToPosix(&context->Mem, options->WorkingDirectory);
    if (!AppendCStrLimited(&prompt, "\nCurrent working directory: ") ||
        !AppendLimited(&prompt, cwd))
      goto too_large;
  }
  context->SystemPrompt = StrBuf_View(&prompt);
  return true;

too_large:
  StrBuf_AppendCStr(error, "assembled system prompt exceeds the 16 MiB limit");
  return false;
}

}  // namespace

bool PromptContext_Build(const PromptContextOptions* options, PromptContext* out,
                         StrBuf* error) {
  if (!options || !out || !error || !options->WorkingDirectory.Length ||
      (options->ToolCount && !options->Tools) ||
      (options->SkillPathCount && !options->SkillPaths) ||
      (options->SettingsSkillPathCount && !options->SettingsSkillPaths) ||
      (options->PromptTemplatePathCount && !options->PromptTemplatePaths) ||
      (options->SettingsPromptTemplatePathCount &&
       !options->SettingsPromptTemplatePaths) ||
      (options->ExplicitAppendPromptCount &&
       !options->ExplicitAppendPrompts)) {
    if (error) StrBuf_AppendCStr(error, "invalid prompt context options");
    return false;
  }

  *out = PromptContext{};
  out->Mem = ArenaCreate((usize)128 << 20);
  out->Initialized = true;
  out->ContextFiles = Array_Make<PromptContextFile>(&out->Mem, 8);
  out->Diagnostics = Array_Make<PromptDiagnostic>(&out->Mem, 4);
  if (options->SettingsSkillPathCount) {
    out->SettingsSkillPaths =
        Array_Make<StrView>(&out->Mem, options->SettingsSkillPathCount);
    for (u32 i = 0; i < options->SettingsSkillPathCount; i++)
      Array_Push(&out->SettingsSkillPaths,
                 StrView_Dup(&out->Mem, options->SettingsSkillPaths[i]));
  }
  if (options->SettingsPromptTemplatePathCount) {
    out->SettingsPromptTemplatePaths =
        Array_Make<StrView>(&out->Mem, options->SettingsPromptTemplatePathCount);
    for (u32 i = 0; i < options->SettingsPromptTemplatePathCount; i++)
      Array_Push(&out->SettingsPromptTemplatePaths,
                 StrView_Dup(&out->Mem, options->SettingsPromptTemplatePaths[i]));
  }

  DiscoverContextFiles(out, options);
  DiscoverSkills(out, options);
  DiscoverPromptTemplates(out, options);
  StrView basePrompt = {};
  StrView appendPrompt = {};
  if (!ResolveSystemPrompt(out, options, &basePrompt, error) ||
      !ResolveAppendPrompt(out, options, &appendPrompt, error) ||
      !BuildFinalPrompt(out, options, basePrompt, appendPrompt, error)) {
    PromptContext_Release(out);
    return false;
  }
  return true;
}

void PromptContext_Release(PromptContext* context) {
  if (!context || !context->Initialized) return;
  ArenaRelease(&context->Mem);
  *context = PromptContext{};
}
