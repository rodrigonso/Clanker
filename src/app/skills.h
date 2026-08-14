#pragma once
#include "base/arena.h"
#include "base/array.h"
#include "base/str.h"

#include "app/loader.h"

struct Skill {
  StrView Name;
  StrView Description;
  StrView FilePath;
  StrView BaseDir;
  bool DisableModelInvocation;
};

struct SkillSet {
  Array<Skill> Skills;
  Array<LoadDiagnostic> Diagnostics;
};

struct SkillLoadOptions {
  StrView AgentDirectory;
  StrView WorkingDirectory;
  bool ProjectTrusted;
  bool NoSkills;
  const StrView* SettingsPaths;
  u32 SettingsPathCount;
  const StrView* ExplicitPaths;
  u32 ExplicitPathCount;
};

// Results are owned by `arena`. The first skill to claim a name wins.
void Skills_Load(Arena* arena, const SkillLoadOptions* options, SkillSet* out);

bool Skills_ParseFile(Arena* arena, StrView filePath, StrView content,
                      Skill* out, Array<LoadDiagnostic>* warnings);

StrView Skills_ExtractBody(Arena* arena, StrView content);

const Skill* Skills_Find(const SkillSet* skills, StrView name);

// Appends pi's `<available_skills>` block. Returns false without appending a
// partial block when the result would exceed `maxBytes`.
bool Skills_AppendPromptBlock(StrBuf* out, const SkillSet* skills, u32 maxBytes);

StrView Skills_FormatInvocation(Arena* arena, const Skill* skill, StrView body,
                                StrView arguments);
