#pragma once

#include "agent/tool.h"
#include "app/prompt_templates.h"
#include "app/skills.h"
#include "base/array.h"
#include "base/str.h"

enum class PromptDiagnosticSeverity : u8 { Warning, Error };

struct PromptDiagnostic {
  PromptDiagnosticSeverity Severity;
  StrView Path;
  StrView Message;
};

struct PromptContextFile {
  StrView Path;
  StrView Content;
};

struct PromptContextOptions {
  StrView WorkingDirectory;
  StrView AgentDirectory;
  const Tool* Tools;
  u32 ToolCount;
  bool NoContextFiles;
  bool NoSkills;
  bool NoPromptTemplates;
  bool ProjectTrusted;
  bool HasExplicitSystemPrompt;
  StrView ExplicitSystemPrompt;
  const StrView* ExplicitAppendPrompts;
  u32 ExplicitAppendPromptCount;
  const StrView* SkillPaths;
  u32 SkillPathCount;
  const StrView* SettingsSkillPaths;
  u32 SettingsSkillPathCount;
  const StrView* PromptTemplatePaths;
  u32 PromptTemplatePathCount;
  const StrView* SettingsPromptTemplatePaths;
  u32 SettingsPromptTemplatePathCount;
  // Folder holding every repository's memory wiki (normally
  // `~/.clanker/memory`). Empty disables memory, leaving the prompt exactly as
  // it is without the feature.
  StrView MemoryStore;
};

struct PromptContext {
  Arena Mem;
  bool Initialized;
  StrView SystemPrompt;
  StrView SystemPromptSource;
  StrView AppendPromptSource;
  Array<PromptContextFile> ContextFiles;
  SkillSet Skills;
  Array<StrView> SettingsSkillPaths;
  // Templates are never shown to the model; they live here so `/reload` swaps
  // them atomically with everything else the project can contribute.
  PromptTemplateSet Templates;
  Array<StrView> SettingsPromptTemplatePaths;
  // The working directory's wiki pages folder when memory is enabled, and the
  // `<memory_wiki>` block built from it. The block is absent without `read`.
  StrView MemoryPages;
  StrView MemoryBlock;
  u32 MemoryPageCount;
  Array<PromptDiagnostic> Diagnostics;
};

// Builds a fully owned prompt context. On failure `out` remains releasable and
// `error` receives a human-readable reason.
bool PromptContext_Build(const PromptContextOptions* options, PromptContext* out,
                         StrBuf* error);
void PromptContext_Release(PromptContext* context);
