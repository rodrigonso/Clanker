#pragma once

#include "agent/tool.h"
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
  bool ProjectTrusted;
  bool HasExplicitSystemPrompt;
  StrView ExplicitSystemPrompt;
  const StrView* ExplicitAppendPrompts;
  u32 ExplicitAppendPromptCount;
};

struct PromptContext {
  Arena Mem;
  bool Initialized;
  StrView SystemPrompt;
  StrView SystemPromptSource;
  StrView AppendPromptSource;
  Array<PromptContextFile> ContextFiles;
  Array<PromptDiagnostic> Diagnostics;
};

// Builds a fully owned prompt context. On failure `out` remains releasable and
// `error` receives a human-readable reason.
bool PromptContext_Build(const PromptContextOptions* options, PromptContext* out,
                         StrBuf* error);
void PromptContext_Release(PromptContext* context);

