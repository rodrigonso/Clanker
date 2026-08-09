#include "app/skills.h"

#include "app/frontmatter.h"
#include "base/config_file.h"
#include "tools/fs.h"

namespace {

constexpr u64 kMaxSkillFileBytes = (u64)1 << 20;
constexpr u32 kMaxSkillNameBytes = 64;
constexpr u32 kMaxSkillDescriptionBytes = 1024;
constexpr u32 kMaxSkillDepth = 8;

const StrView kIgnoreFiles[] = {SVL(".gitignore"), SVL(".ignore"),
                                SVL(".fdignore")};
constexpr StrView kPromptPrefix =
    SVL("\n\nThe following skills provide specialized instructions for "
        "specific tasks.\n"
        "Use the read tool to load a skill's file when the task matches "
        "its description.\n"
        "When a skill file references a relative path, resolve it "
        "against the skill directory (parent of SKILL.md / dirname of "
        "the path) and use that absolute path in tool commands.\n"
        "\n<available_skills>\n");
constexpr StrView kPromptSuffix = SVL("</available_skills>");

bool IsMarkdown(StrView name) {
  return name.Length > 3 &&
         StrView_EndsWithInsensitiveAscii(name, SVL(".md"));
}

StrView AppendRel(Arena* arena, StrView parent, StrView name) {
  StrView n = Fs_ToPosix(arena, name);
  if (!parent.Length) return n;
  StrBuf out = StrBuf_Make(arena, parent.Length + n.Length + 2);
  StrBuf_Append(&out, parent);
  StrBuf_AppendChar(&out, '/');
  StrBuf_Append(&out, n);
  return StrBuf_View(&out);
}

void AddDiagnostic(Arena* arena, Array<SkillDiagnostic>* diagnostics,
                   StrView path, StrView message) {
  if (!diagnostics) return;
  SkillDiagnostic diagnostic = {};
  diagnostic.Path = StrView_Dup(arena, path);
  diagnostic.Message = StrView_Dup(arena, message);
  Array_Push(diagnostics, diagnostic);
}

// Skills need only these top-level scalars, not a general YAML parser.
struct Frontmatter {
  StrView Name;
  StrView Description;
  bool DisableModelInvocation;
};

Frontmatter ParseSkillFrontmatter(Arena* arena, StrView content,
                                  StrView* body) {
  FrontmatterField fields[] = {{SVL("name"), {}},
                               {SVL("description"), {}},
                               {SVL("disable-model-invocation"), {}}};
  Frontmatter_Parse(arena, content, fields, 3, body);
  Frontmatter out = {};
  out.Name = fields[0].Value;
  out.Description = fields[1].Value;
  out.DisableModelInvocation = Frontmatter_ParseBool(fields[2].Value);
  return out;
}

void ValidateName(Arena* arena, StrView path, StrView name,
                  Array<SkillDiagnostic>* warnings) {
  if (!name.Length) return;
  if (name.Length > kMaxSkillNameBytes) {
    AddDiagnostic(arena, warnings, path,
                  SVL("skill name exceeds 64 characters"));
  }
  for (u32 i = 0; i < name.Length; i++) {
    char c = name.Data[i];
    bool allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    if (allowed) continue;
    AddDiagnostic(arena, warnings, path,
                  SVL("skill name should use only lowercase letters, digits, "
                      "and hyphens"));
    break;
  }
  if (name.Data[0] == '-' || name.Data[name.Length - 1] == '-') {
    AddDiagnostic(arena, warnings, path,
                  SVL("skill name should not start or end with a hyphen"));
  }
  for (u32 i = 1; i < name.Length; i++) {
    if (name.Data[i] != '-' || name.Data[i - 1] != '-') continue;
    AddDiagnostic(arena, warnings, path,
                  SVL("skill name should not contain consecutive hyphens"));
    break;
  }
}

struct LoadContext {
  Arena* Mem;
  SkillSet* Out;
  Array<StrView> SeenPaths;
};

bool AlreadyVisited(LoadContext* ctx, StrView canonical) {
  for (u32 i = 0; i < ctx->SeenPaths.Count; i++)
    if (Fs_PathEquals(ctx->SeenPaths.Data[i], canonical)) return true;
  return false;
}

void LoadSkillFile(LoadContext* ctx, Arena* scratch, StrView path) {
  usize mark = scratch->Used;
  StrView canonical = Fs_RealPath(scratch, path);
  if (AlreadyVisited(ctx, canonical)) {
    ArenaRestore(scratch, mark);
    return;
  }
  canonical = StrView_Dup(ctx->Mem, canonical);
  Array_Push(&ctx->SeenPaths, canonical);

  FsStat stat = Fs_Stat(canonical);
  if (!stat.Exists || stat.IsDir) {
    ArenaRestore(scratch, mark);
    return;
  }
  if (stat.Size > kMaxSkillFileBytes) {
    AddDiagnostic(ctx->Mem, &ctx->Out->Diagnostics, canonical,
                  SVL("skill file exceeds the 1 MiB limit"));
    ArenaRestore(scratch, mark);
    return;
  }

  StrView content = {};
  if (ConfigFile_ReadAll(scratch, canonical, &content) !=
      ConfigFileReadResult::Success) {
    AddDiagnostic(ctx->Mem, &ctx->Out->Diagnostics, canonical,
                  SVL("skill file could not be read"));
    ArenaRestore(scratch, mark);
    return;
  }

  Skill parsed = {};
  Array<SkillDiagnostic> diagnostics =
      Array_Make<SkillDiagnostic>(scratch, 4);
  bool parsedOk =
      Skills_ParseFile(scratch, canonical, content, &parsed, &diagnostics);
  for (u32 i = 0; i < diagnostics.Count; i++) {
    AddDiagnostic(ctx->Mem, &ctx->Out->Diagnostics,
                  diagnostics.Data[i].Path, diagnostics.Data[i].Message);
  }
  if (!parsedOk) {
    ArenaRestore(scratch, mark);
    return;
  }

  const Skill* existing = Skills_Find(ctx->Out, parsed.Name);
  if (existing) {
    StrBuf message = StrBuf_Make(ctx->Mem, 96 + existing->FilePath.Length);
    StrBuf_AppendCStr(&message, "skill name '");
    StrBuf_Append(&message, parsed.Name);
    StrBuf_AppendCStr(&message, "' is already defined by ");
    StrBuf_Append(&message, existing->FilePath);
    AddDiagnostic(ctx->Mem, &ctx->Out->Diagnostics, canonical,
                  StrBuf_View(&message));
    ArenaRestore(scratch, mark);
    return;
  }

  Skill skill = {};
  skill.Name = StrView_Dup(ctx->Mem, parsed.Name);
  skill.Description = StrView_Dup(ctx->Mem, parsed.Description);
  skill.FilePath = StrView_Dup(ctx->Mem, parsed.FilePath);
  skill.BaseDir = StrView_Dup(ctx->Mem, parsed.BaseDir);
  skill.DisableModelInvocation = parsed.DisableModelInvocation;
  Array_Push(&ctx->Out->Skills, skill);
  ArenaRestore(scratch, mark);
}

void LoadSkillsFromDir(LoadContext* ctx, Arena* scratch, StrView dirAbs,
                       StrView dirRel, bool includeRootFiles,
                       const Array<FsIgnorePattern>* inherited, u32 depth) {
  if (depth > kMaxSkillDepth) return;
  FsStat dirStat = Fs_Stat(dirAbs);
  if (!dirStat.Exists || !dirStat.IsDir) return;

  usize mark = scratch->Used;
  Array<FsIgnorePattern> patterns =
      Array_Make<FsIgnorePattern>(scratch, inherited->Count + 8);
  for (u32 i = 0; i < inherited->Count; i++)
    Array_Push(&patterns, inherited->Data[i]);
  for (u32 i = 0; i < sizeof(kIgnoreFiles) / sizeof(kIgnoreFiles[0]); i++)
    Fs_LoadIgnoreFile(scratch, &patterns, dirAbs, dirRel, kIgnoreFiles[i]);

  StrView skillFile = Fs_Join(scratch, dirAbs, SVL("SKILL.md"));
  FsStat skillStat = Fs_Stat(skillFile);
  if (skillStat.Exists && !skillStat.IsDir) {
    LoadSkillFile(ctx, scratch, skillFile);
    ArenaRestore(scratch, mark);
    return;
  }

  bool ok = false;
  Array<FsEntry> entries = Fs_ListDir(scratch, dirAbs, &ok);
  if (!ok) {
    ArenaRestore(scratch, mark);
    return;
  }
  for (u32 i = 0; i < entries.Count; i++) {
    StrView name = entries.Data[i].Name;
    if (!name.Length || name.Data[0] == '.') continue;
    if (StrView_EqCStr(name, "node_modules")) continue;

    StrView abs = Fs_Join(scratch, dirAbs, name);
    StrView rel = AppendRel(scratch, dirRel, name);
    FsStat stat = Fs_Stat(abs);
    if (!stat.Exists) continue;
    if (Fs_IsIgnored(&patterns, rel, stat.IsDir)) continue;

    if (stat.IsDir)
      LoadSkillsFromDir(ctx, scratch, abs, rel, false, &patterns, depth + 1);
    else if (includeRootFiles && IsMarkdown(name))
      LoadSkillFile(ctx, scratch, abs);
  }
  ArenaRestore(scratch, mark);
}

void LoadRoot(LoadContext* ctx, Arena* scratch, StrView dirAbs) {
  usize mark = scratch->Used;
  Array<FsIgnorePattern> empty = {};
  LoadSkillsFromDir(ctx, scratch, dirAbs, StrView{"", 0}, true, &empty, 0);
  ArenaRestore(scratch, mark);
}

void LoadExplicitPath(LoadContext* ctx, Arena* scratch, StrView path) {
  if (!path.Length) return;
  FsStat stat = Fs_Stat(path);
  if (!stat.Exists) {
    AddDiagnostic(ctx->Mem, &ctx->Out->Diagnostics, path,
                  SVL("skill path does not exist"));
    return;
  }
  if (stat.IsDir) {
    LoadRoot(ctx, scratch, path);
    return;
  }
  LoadSkillFile(ctx, scratch, path);
}

// `.agents/skills` is shared with other harnesses and may sit at any level
// between the repository root and the working directory, so every ancestor is
// checked. Broad directories are visited first, matching how context files
// layer, and the first definition of a name still wins.
void LoadAgentsAncestors(LoadContext* ctx, Arena* scratch,
                         StrView workingDirectory) {
  usize mark = scratch->Used;
  StrView cwd = Fs_RealPath(scratch, workingDirectory);
  Array<StrView> ancestors = Array_Make<StrView>(scratch, 16);
  StrView current = cwd;
  while (current.Length) {
    Array_Push(&ancestors, current);
    StrView git = Fs_Join(scratch, current, SVL(".git"));
    if (Fs_Exists(git)) break;
    StrView parent = Fs_Parent(current);
    if (!parent.Length) break;
    current = StrView_Dup(scratch, parent);
  }
  while (ancestors.Count) {
    StrView directory = Array_Pop(&ancestors);
    StrView agents = Fs_Join(scratch, directory, SVL(".agents"));
    LoadRoot(ctx, scratch, Fs_Join(scratch, agents, SVL("skills")));
  }
  ArenaRestore(scratch, mark);
}

// pi escapes five characters in skill metadata, one more than the four the
// project-context framing escapes. Reusing that escaper would drop `&apos;`.
u64 EscapedLength(StrView value) {
  u64 length = value.Length;
  for (u32 i = 0; i < value.Length; i++) {
    switch (value.Data[i]) {
      case '&': length += 4; break;
      case '<':
      case '>': length += 3; break;
      case '"':
      case '\'': length += 5; break;
    }
  }
  return length;
}

void AppendEscaped(StrBuf* out, StrView value) {
  for (u32 i = 0; i < value.Length; i++) {
    switch (value.Data[i]) {
      case '&': StrBuf_AppendCStr(out, "&amp;"); break;
      case '<': StrBuf_AppendCStr(out, "&lt;"); break;
      case '>': StrBuf_AppendCStr(out, "&gt;"); break;
      case '"': StrBuf_AppendCStr(out, "&quot;"); break;
      case '\'': StrBuf_AppendCStr(out, "&apos;"); break;
      default: StrBuf_AppendChar(out, value.Data[i]); break;
    }
  }
}

}  // namespace

bool Skills_ParseFile(Arena* arena, StrView filePath, StrView content,
                      Skill* out, Array<SkillDiagnostic>* warnings) {
  *out = Skill{};
  Frontmatter meta = ParseSkillFrontmatter(arena, content, nullptr);

  StrView name = StrView_Trim(meta.Name);
  if (!name.Length) name = Fs_Base(Fs_Parent(filePath));
  if (!name.Length) {
    AddDiagnostic(arena, warnings, filePath, SVL("skill has no usable name"));
    return false;
  }
  StrView description = StrView_Trim(meta.Description);
  if (!description.Length) {
    AddDiagnostic(arena, warnings, filePath,
                  SVL("skill is missing a frontmatter description"));
    return false;
  }
  if (description.Length > kMaxSkillDescriptionBytes) {
    AddDiagnostic(arena, warnings, filePath,
                  SVL("skill description exceeds 1024 characters"));
  }
  ValidateName(arena, filePath, name, warnings);

  out->Name = StrView_Dup(arena, name);
  out->Description = StrView_Dup(arena, description);
  out->FilePath = Fs_ToPosix(arena, filePath);
  out->BaseDir = Fs_ToPosix(arena, Fs_Parent(filePath));
  out->DisableModelInvocation = meta.DisableModelInvocation;
  return true;
}

StrView Skills_ExtractBody(Arena* arena, StrView content) {
  StrView body = {};
  Frontmatter_Parse(arena, content, nullptr, 0, &body);
  return body;
}

const Skill* Skills_Find(const SkillSet* skills, StrView name) {
  if (!skills || !name.Length) return nullptr;
  for (u32 i = 0; i < skills->Skills.Count; i++) {
    if (StrView_Eq(skills->Skills.Data[i].Name, name))
      return &skills->Skills.Data[i];
  }
  return nullptr;
}

void Skills_Load(Arena* arena, const SkillLoadOptions* options, SkillSet* out) {
  *out = SkillSet{};
  out->Skills = Array_Make<Skill>(arena, 8);
  out->Diagnostics = Array_Make<SkillDiagnostic>(arena, 4);
  if (!options) return;

  LoadContext ctx = {};
  ctx.Mem = arena;
  ctx.Out = out;
  ctx.SeenPaths = Array_Make<StrView>(arena, 16);

  Arena scratch = ArenaCreate((usize)8 << 20);
  if (!options->NoSkills) {
    if (options->AgentDirectory.Length) {
      LoadRoot(&ctx, &scratch,
               Fs_Join(&scratch, options->AgentDirectory, SVL("skills")));
      StrView home = Fs_Parent(options->AgentDirectory);
      if (home.Length) {
        StrView agents = Fs_Join(&scratch, home, SVL(".agents"));
        LoadRoot(&ctx, &scratch, Fs_Join(&scratch, agents, SVL("skills")));
      }
    }
    if (options->ProjectTrusted && options->WorkingDirectory.Length) {
      LoadAgentsAncestors(&ctx, &scratch, options->WorkingDirectory);
      StrView config =
          Fs_Join(&scratch, options->WorkingDirectory, SVL(".clank"));
      LoadRoot(&ctx, &scratch, Fs_Join(&scratch, config, SVL("skills")));
    }
    for (u32 i = 0; i < options->SettingsPathCount; i++)
      LoadExplicitPath(&ctx, &scratch, options->SettingsPaths[i]);
  }

  for (u32 i = 0; i < options->ExplicitPathCount; i++)
    LoadExplicitPath(&ctx, &scratch, options->ExplicitPaths[i]);
  ArenaRelease(&scratch);
}

bool Skills_AppendPromptBlock(StrBuf* out, const SkillSet* skills,
                              u32 maxBytes) {
  if (!out || !skills) return true;
  u64 required = kPromptPrefix.Length + kPromptSuffix.Length;
  u32 visible = 0;
  for (u32 i = 0; i < skills->Skills.Count; i++) {
    const Skill& skill = skills->Skills.Data[i];
    if (skill.DisableModelInvocation) continue;
    visible++;
    required += SVL("  <skill>\n    <name>").Length +
                EscapedLength(skill.Name) +
                SVL("</name>\n    <description>").Length +
                EscapedLength(skill.Description) +
                SVL("</description>\n    <location>").Length +
                EscapedLength(skill.FilePath) +
                SVL("</location>\n  </skill>\n").Length;
  }
  if (!visible) return true;
  if ((u64)out->Length + required > maxBytes) return false;

  StrBuf_Append(out, kPromptPrefix);
  for (u32 i = 0; i < skills->Skills.Count; i++) {
    const Skill& skill = skills->Skills.Data[i];
    if (skill.DisableModelInvocation) continue;
    StrBuf_AppendCStr(out, "  <skill>\n    <name>");
    AppendEscaped(out, skill.Name);
    StrBuf_AppendCStr(out, "</name>\n    <description>");
    AppendEscaped(out, skill.Description);
    StrBuf_AppendCStr(out, "</description>\n    <location>");
    AppendEscaped(out, skill.FilePath);
    StrBuf_AppendCStr(out, "</location>\n  </skill>\n");
  }
  StrBuf_Append(out, kPromptSuffix);
  return true;
}

StrView Skills_FormatInvocation(Arena* arena, const Skill* skill, StrView body,
                                StrView arguments) {
  if (!skill) return StrView{"", 0};
  StrBuf out = StrBuf_Make(arena, body.Length + arguments.Length + 256);
  StrBuf_AppendCStr(&out, "<skill name=\"");
  StrBuf_Append(&out, skill->Name);
  StrBuf_AppendCStr(&out, "\" location=\"");
  StrBuf_Append(&out, skill->FilePath);
  StrBuf_AppendCStr(&out, "\">\nReferences are relative to ");
  StrBuf_Append(&out, skill->BaseDir);
  StrBuf_AppendCStr(&out, ".\n\n");
  StrBuf_Append(&out, body);
  StrBuf_AppendCStr(&out, "\n</skill>");
  StrView trimmedArguments = StrView_Trim(arguments);
  if (trimmedArguments.Length) {
    StrBuf_AppendCStr(&out, "\n\n");
    StrBuf_Append(&out, trimmedArguments);
  }
  return StrBuf_View(&out);
}
