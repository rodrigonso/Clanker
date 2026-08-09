#include "app/prompt_templates.h"

#include "app/frontmatter.h"
#include "base/config_file.h"
#include "tools/fs.h"

namespace {

constexpr u64 kMaxTemplateFileBytes = (u64)1 << 20;
// pi truncates a fallback description at 60 characters and marks the cut.
constexpr u32 kDescriptionFallbackBytes = 60;

bool HasMarkdownSuffix(StrView name) {
  return StrView_EndsWithInsensitiveAscii(name, SVL(".md"));
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

void AddDiagnostic(Arena* arena, Array<PromptTemplateDiagnostic>* diagnostics,
                   StrView path, StrView message) {
  if (!diagnostics) return;
  PromptTemplateDiagnostic diagnostic = {};
  diagnostic.Path = StrView_Dup(arena, path);
  diagnostic.Message = StrView_Dup(arena, message);
  Array_Push(diagnostics, diagnostic);
}

StrView NameFromPath(StrView filePath) {
  StrView base = Fs_Base(filePath);
  if (HasMarkdownSuffix(base)) base.Length -= 3;
  return base;
}

// Leading whitespace counts toward pi's 60-byte fallback cutoff.
StrView FallbackDescription(Arena* arena, StrView body) {
  u32 pos = 0;
  while (pos <= body.Length) {
    u32 start = pos;
    u32 end = start;
    while (end < body.Length && body.Data[end] != '\n') end++;
    StrView line = StrView{body.Data + start, end - start};
    pos = end + 1;
    if (!StrView_Trim(line).Length) {
      if (end >= body.Length) break;
      continue;
    }
    if (line.Length <= kDescriptionFallbackBytes) return line;
    StrBuf out = StrBuf_Make(arena, kDescriptionFallbackBytes + 4);
    StrBuf_Append(&out, StrView{line.Data, kDescriptionFallbackBytes});
    StrBuf_AppendCStr(&out, "...");
    return StrBuf_View(&out);
  }
  return StrView{};
}

struct LoadContext {
  Arena* Mem;
  PromptTemplateSet* Out;
  Array<StrView> SeenPaths;
};

bool AlreadyVisited(LoadContext* ctx, StrView canonical) {
  for (u32 i = 0; i < ctx->SeenPaths.Count; i++)
    if (Fs_PathEquals(ctx->SeenPaths.Data[i], canonical)) return true;
  return false;
}

void LoadTemplateFile(LoadContext* ctx, Arena* scratch, StrView path) {
  usize mark = scratch->Used;
  StrView canonical = Fs_RealPath(scratch, path);
  if (!canonical.Length) canonical = path;
  if (AlreadyVisited(ctx, canonical)) {
    ArenaRestore(scratch, mark);
    return;
  }
  Array_Push(&ctx->SeenPaths, StrView_Dup(ctx->Mem, canonical));

  FsStat stat = Fs_Stat(canonical);
  if (!stat.Exists || stat.IsDir) {
    ArenaRestore(scratch, mark);
    return;
  }
  if (stat.Size > kMaxTemplateFileBytes) {
    AddDiagnostic(ctx->Mem, &ctx->Out->Diagnostics, canonical,
                  SVL("prompt template exceeds the 1 MiB limit"));
    ArenaRestore(scratch, mark);
    return;
  }

  StrView content = {};
  if (ConfigFile_ReadAll(scratch, canonical, &content) !=
      ConfigFileReadResult::Success) {
    AddDiagnostic(ctx->Mem, &ctx->Out->Diagnostics, canonical,
                  SVL("prompt template could not be read"));
    ArenaRestore(scratch, mark);
    return;
  }

  PromptTemplate parsed = {};
  if (!PromptTemplates_ParseFile(scratch, canonical, content, &parsed)) {
    AddDiagnostic(ctx->Mem, &ctx->Out->Diagnostics, canonical,
                  SVL("prompt template has no usable name"));
    ArenaRestore(scratch, mark);
    return;
  }

  const PromptTemplate* existing = PromptTemplates_Find(ctx->Out, parsed.Name);
  if (existing) {
    StrBuf message = StrBuf_Make(ctx->Mem, 96 + existing->FilePath.Length);
    StrBuf_AppendCStr(&message, "prompt template '");
    StrBuf_Append(&message, parsed.Name);
    StrBuf_AppendCStr(&message, "' is already defined by ");
    StrBuf_Append(&message, existing->FilePath);
    AddDiagnostic(ctx->Mem, &ctx->Out->Diagnostics, canonical,
                  StrBuf_View(&message));
    ArenaRestore(scratch, mark);
    return;
  }

  PromptTemplate tmpl = {};
  tmpl.Name = StrView_Dup(ctx->Mem, parsed.Name);
  tmpl.Description = StrView_Dup(ctx->Mem, parsed.Description);
  tmpl.ArgumentHint = StrView_Dup(ctx->Mem, parsed.ArgumentHint);
  tmpl.FilePath = StrView_Dup(ctx->Mem, parsed.FilePath);
  // Unlike skills, bodies are retained: expansion happens without touching the
  // disk again, and templates are small by construction.
  tmpl.Content = StrView_Dup(ctx->Mem, parsed.Content);
  Array_Push(&ctx->Out->Templates, tmpl);
  ArenaRestore(scratch, mark);
}

void LoadTemplateDir(LoadContext* ctx, Arena* scratch, StrView dirAbs) {
  usize mark = scratch->Used;
  FsStat dirStat = Fs_Stat(dirAbs);
  if (!dirStat.Exists || !dirStat.IsDir) {
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
    if (entries.Data[i].IsDir || name.Length <= 3 ||
        !HasMarkdownSuffix(name))
      continue;
    LoadTemplateFile(ctx, scratch, Fs_Join(scratch, dirAbs, name));
  }
  ArenaRestore(scratch, mark);
}

void LoadExplicitPath(LoadContext* ctx, Arena* scratch, StrView path) {
  if (!path.Length) return;
  FsStat stat = Fs_Stat(path);
  if (!stat.Exists) {
    AddDiagnostic(ctx->Mem, &ctx->Out->Diagnostics, path,
                  SVL("prompt template path does not exist"));
    return;
  }
  if (stat.IsDir)
    LoadTemplateDir(ctx, scratch, path);
  else
    LoadTemplateFile(ctx, scratch, path);
}

StrView JoinArgs(Arena* arena, const StrView* args, u32 argCount) {
  if (!argCount) return StrView{"", 0};
  if (argCount == 1) return args[0];
  u32 total = 0;
  for (u32 i = 0; i < argCount; i++) total += args[i].Length + 1;
  StrBuf out = StrBuf_Make(arena, total);
  for (u32 i = 0; i < argCount; i++) {
    if (i) StrBuf_AppendChar(&out, ' ');
    StrBuf_Append(&out, args[i]);
  }
  return StrBuf_View(&out);
}

void AppendSlice(StrBuf* out, const StrView* args, u32 argCount, u32 start,
                 u32 count) {
  bool first = true;
  // Widened because an absent length is represented as the u32 maximum, which
  // would wrap `start + count` back below `start`.
  u64 end = (u64)start + count;
  for (u64 i = start; i < argCount && i < end; i++) {
    if (!first) StrBuf_AppendChar(out, ' ');
    StrBuf_Append(out, args[i]);
    first = false;
  }
}

// Parses a run of digits, saturating rather than wrapping so a pathological
// index cannot alias a real argument.
u32 ParseIndex(StrView text, u32* pos) {
  u64 value = 0;
  while (*pos < text.Length && IsDigit(text.Data[*pos])) {
    if (value <= 0xFFFFFFFFull) value = value * 10 + (u64)(text.Data[*pos] - '0');
    (*pos)++;
  }
  return value > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)value;
}

// `${...}` forms. `pos` points just past the `{`; on success it is advanced
// past the closing `}` and the expansion is appended.
bool ExpandBraced(StrBuf* out, StrView content, u32* pos, const StrView* args,
                  u32 argCount, StrView allArgs) {
  u32 cursor = *pos;
  bool isAll = false;
  u32 index = 0;

  if (cursor < content.Length && content.Data[cursor] == '@') {
    isAll = true;
    cursor++;
  } else if (cursor + 9 <= content.Length &&
             memcmp(content.Data + cursor, "ARGUMENTS", 9) == 0) {
    isAll = true;
    cursor += 9;
  } else if (cursor < content.Length && IsDigit(content.Data[cursor])) {
    index = ParseIndex(content, &cursor);
  } else {
    return false;
  }

  // `${@:N}` / `${@:N:L}` slicing, which pi accepts only for `@`.
  if (isAll && cursor + 1 < content.Length && content.Data[cursor] == ':' &&
      IsDigit(content.Data[cursor + 1])) {
    cursor++;
    u32 sliceStart = ParseIndex(content, &cursor);
    // Users write 1-based indices; bash treats 0 as 1.
    u32 start = sliceStart ? sliceStart - 1 : 0;
    u32 count = 0xFFFFFFFFu;
    if (cursor < content.Length && content.Data[cursor] == ':') {
      cursor++;
      if (cursor >= content.Length || !IsDigit(content.Data[cursor])) return false;
      count = ParseIndex(content, &cursor);
    }
    if (cursor >= content.Length || content.Data[cursor] != '}') return false;
    AppendSlice(out, args, argCount, start, count);
    *pos = cursor + 1;
    return true;
  }

  if (cursor + 1 >= content.Length || content.Data[cursor] != ':' ||
      content.Data[cursor + 1] != '-')
    return false;
  cursor += 2;
  u32 defaultStart = cursor;
  while (cursor < content.Length && content.Data[cursor] != '}') cursor++;
  if (cursor >= content.Length) return false;
  StrView fallback = StrView{content.Data + defaultStart, cursor - defaultStart};

  StrView value = isAll ? allArgs
                        : (index && index <= argCount ? args[index - 1]
                                                      : StrView{});
  // pi tests the resolved value for truthiness, so an argument that was
  // supplied but empty also falls back to the default.
  StrBuf_Append(out, value.Length ? value : fallback);
  *pos = cursor + 1;
  return true;
}

}  // namespace

bool PromptTemplates_ParseFile(Arena* arena, StrView filePath, StrView content,
                               PromptTemplate* out) {
  *out = PromptTemplate{};
  StrView name = NameFromPath(filePath);
  if (!name.Length) return false;

  StrView body = {};
  FrontmatterField fields[] = {{SVL("description"), {}},
                               {SVL("argument-hint"), {}}};
  Frontmatter_Parse(arena, content, fields, 2, &body);

  StrView description = StrView_Trim(fields[0].Value);
  if (!description.Length) description = FallbackDescription(arena, body);

  out->Name = StrView_Dup(arena, name);
  out->Description = StrView_Dup(arena, description);
  out->ArgumentHint = StrView_Dup(arena, StrView_Trim(fields[1].Value));
  out->FilePath = Fs_ToPosix(arena, filePath);
  out->Content = StrView_Dup(arena, body);
  return true;
}

const PromptTemplate* PromptTemplates_Find(const PromptTemplateSet* templates,
                                           StrView name) {
  if (!templates || !name.Length) return nullptr;
  for (u32 i = 0; i < templates->Templates.Count; i++) {
    if (StrView_Eq(templates->Templates.Data[i].Name, name))
      return &templates->Templates.Data[i];
  }
  return nullptr;
}

void PromptTemplates_SplitArgs(Arena* arena, StrView argsString,
                               Array<StrView>* out) {
  if (!out) return;
  if (!out->Capacity) *out = Array_Make<StrView>(arena, 8);
  StrBuf current = StrBuf_Make(arena, 32);
  bool pending = false;
  char quote = 0;
  for (u32 i = 0; i < argsString.Length; i++) {
    char c = argsString.Data[i];
    if (quote) {
      if (c == quote)
        quote = 0;
      else
        StrBuf_AppendChar(&current, c);
      // An empty quoted string is still an argument, so track it separately
      // from the buffer length.
      pending = true;
    } else if (c == '"' || c == '\'') {
      quote = c;
      pending = true;
    } else if (Ascii_IsSpace(c)) {
      if (pending) {
        Array_Push(out, StrBuf_View(&current));
        current = StrBuf_Make(arena, 32);
        pending = false;
      }
    } else {
      StrBuf_AppendChar(&current, c);
      pending = true;
    }
  }
  if (pending) Array_Push(out, StrBuf_View(&current));
}

StrView PromptTemplates_Substitute(Arena* arena, StrView content,
                                   const StrView* args, u32 argCount) {
  StrView allArgs = JoinArgs(arena, args, argCount);
  StrBuf out = StrBuf_Make(arena, content.Length + allArgs.Length + 16);
  u32 i = 0;
  while (i < content.Length) {
    if (content.Data[i] != '$' || i + 1 >= content.Length) {
      StrBuf_AppendChar(&out, content.Data[i]);
      i++;
      continue;
    }
    u32 cursor = i + 1;
    if (content.Data[cursor] == '{') {
      cursor++;
      if (ExpandBraced(&out, content, &cursor, args, argCount, allArgs)) {
        i = cursor;
        continue;
      }
      StrBuf_AppendChar(&out, '$');
      i++;
      continue;
    }
    if (content.Data[cursor] == '@') {
      StrBuf_Append(&out, allArgs);
      i = cursor + 1;
      continue;
    }
    if (cursor + 9 <= content.Length &&
        memcmp(content.Data + cursor, "ARGUMENTS", 9) == 0) {
      StrBuf_Append(&out, allArgs);
      i = cursor + 9;
      continue;
    }
    if (IsDigit(content.Data[cursor])) {
      u32 index = ParseIndex(content, &cursor);
      if (index && index <= argCount) StrBuf_Append(&out, args[index - 1]);
      i = cursor;
      continue;
    }
    StrBuf_AppendChar(&out, '$');
    i++;
  }
  return StrBuf_View(&out);
}

StrView PromptTemplates_Expand(Arena* arena, const PromptTemplate* tmpl,
                               StrView argsString) {
  if (!tmpl) return StrView{};
  Array<StrView> args = {};
  PromptTemplates_SplitArgs(arena, argsString, &args);
  return PromptTemplates_Substitute(arena, tmpl->Content, args.Data,
                                    args.Count);
}

void PromptTemplates_Load(Arena* arena, const PromptTemplateLoadOptions* options,
                          PromptTemplateSet* out) {
  *out = PromptTemplateSet{};
  out->Templates = Array_Make<PromptTemplate>(arena, 8);
  out->Diagnostics = Array_Make<PromptTemplateDiagnostic>(arena, 4);
  if (!options) return;

  LoadContext ctx = {};
  ctx.Mem = arena;
  ctx.Out = out;
  ctx.SeenPaths = Array_Make<StrView>(arena, 16);

  Arena scratch = ArenaCreate((usize)8 << 20);
  if (!options->NoPromptTemplates) {
    if (options->AgentDirectory.Length) {
      LoadTemplateDir(&ctx, &scratch,
                      Fs_Join(&scratch, options->AgentDirectory, SVL("prompts")));
    }
    if (options->ProjectTrusted && options->WorkingDirectory.Length) {
      StrView config =
          Fs_Join(&scratch, options->WorkingDirectory, SVL(".clank"));
      LoadTemplateDir(&ctx, &scratch, Fs_Join(&scratch, config, SVL("prompts")));
    }
    for (u32 i = 0; i < options->SettingsPathCount; i++)
      LoadExplicitPath(&ctx, &scratch, options->SettingsPaths[i]);
  }

  for (u32 i = 0; i < options->ExplicitPathCount; i++)
    LoadExplicitPath(&ctx, &scratch, options->ExplicitPaths[i]);
  ArenaRelease(&scratch);
}
