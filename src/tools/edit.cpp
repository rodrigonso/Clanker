#include <stdio.h>
#include <string.h>

#include "json/json.h"
#include "tools/diff.h"
#include "tools/filelock.h"
#include "tools/fs.h"
#include "tools/tools.h"

// Fuzzy matching intentionally omits pi's full NFKC Unicode normalization: this
// C++ port has no Unicode database/ICU. It implements whitespace-run tolerance
// plus the targeted smart-character mappings required by the edit contract.

struct EditSpec {
  StrView OldText;
  StrView NewText;
};

struct EditMatch {
  u32 Start;
  u32 End;
  u32 Index;
};

static void Edit_AppendPathErr(StrBuf* err, const char* prefix, StrView path, const char* suffix) {
  StrBuf_AppendCStr(err, prefix);
  StrBuf_Append(err, path);
  StrBuf_AppendCStr(err, suffix);
}

static StrView Edit_NormalizeLf(Arena* arena, StrView s) {
  StrBuf b = StrBuf_Make(arena, s.Length + 1);
  for (u32 i = 0; i < s.Length; i++) {
    char c = s.Data[i];
    if (c == '\r') {
      if (i + 1 < s.Length && s.Data[i + 1] == '\n') i++;
      StrBuf_AppendChar(&b, '\n');
    } else {
      StrBuf_AppendChar(&b, c);
    }
  }
  return StrBuf_View(&b);
}

static bool Edit_IsCrlf(StrView s) {
  for (u32 i = 0; i < s.Length; i++) {
    if (s.Data[i] == '\r' && i + 1 < s.Length && s.Data[i + 1] == '\n') return true;
    if (s.Data[i] == '\n') return false;
  }
  return false;
}

static StrView Edit_RestoreCrlf(Arena* arena, StrView s) {
  StrBuf b = StrBuf_Make(arena, s.Length + 1);
  for (u32 i = 0; i < s.Length; i++) {
    if (s.Data[i] == '\n') {
      StrBuf_AppendCStr(&b, "\r\n");
    } else {
      StrBuf_AppendChar(&b, s.Data[i]);
    }
  }
  return StrBuf_View(&b);
}

static StrView Edit_PrependBom(Arena* arena, StrView s) {
  StrBuf b = StrBuf_Make(arena, s.Length + 4);
  StrBuf_AppendCStr(&b, "\xEF\xBB\xBF");
  StrBuf_Append(&b, s);
  return StrBuf_View(&b);
}

static bool Edit_ReadFile(Arena* arena, StrView path, StrView* out) {
  const char* p = StrView_DupCStr(arena, path);
  FILE* f = fopen(p, "rb");
  if (!f) return false;

  StrBuf file = StrBuf_Make(arena, 8192);
  char buf[8192];
  size_t n = 0;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    StrBuf_Append(&file, StrView{buf, (u32)n});
  }
  bool ok = ferror(f) == 0;
  fclose(f);
  if (!ok) return false;
  *out = StrBuf_View(&file);
  return true;
}

static bool Edit_WriteFile(Arena* arena, StrView path, StrView content) {
  const char* p = StrView_DupCStr(arena, path);
  FILE* f = fopen(p, "wb");
  if (!f) return false;
  bool ok = true;
  if (content.Length && fwrite(content.Data, 1, content.Length, f) != content.Length) ok = false;
  if (fclose(f) != 0) ok = false;
  return ok;
}

static u32 Edit_FindAllExact(StrView hay, StrView needle, u32* firstStart) {
  if (needle.Length == 0 || needle.Length > hay.Length) return 0;
  u32 count = 0;
  for (u32 i = 0; i + needle.Length <= hay.Length; i++) {
    if (memcmp(hay.Data + i, needle.Data, needle.Length) == 0) {
      if (count == 0 && firstStart) *firstStart = i;
      count++;
    }
  }
  return count;
}

struct EditChar {
  u32 Cp;
  u32 Len;
  u32 Mapped;
};

static EditChar Edit_DecodeMapped(StrView s, u32 pos) {
  EditChar r = {};
  r.Cp = 0;
  r.Len = 0;
  r.Mapped = 0;
  if (pos >= s.Length) return r;

  const u8* p = (const u8*)s.Data + pos;
  u8 b0 = p[0];
  u32 cp = b0;
  u32 len = 1;
  if (b0 >= 0xC2 && b0 <= 0xDF && pos + 1 < s.Length) {
    u8 b1 = p[1];
    if ((b1 & 0xC0) == 0x80) {
      cp = ((u32)(b0 & 0x1F) << 6) | (u32)(b1 & 0x3F);
      len = 2;
    }
  } else if (b0 >= 0xE0 && b0 <= 0xEF && pos + 2 < s.Length) {
    u8 b1 = p[1];
    u8 b2 = p[2];
    bool valid = (b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80;
    if (valid && b0 == 0xE0 && b1 < 0xA0) valid = false;
    if (valid && b0 == 0xED && b1 >= 0xA0) valid = false;
    if (valid) {
      cp = ((u32)(b0 & 0x0F) << 12) | ((u32)(b1 & 0x3F) << 6) | (u32)(b2 & 0x3F);
      len = 3;
    }
  } else if (b0 >= 0xF0 && b0 <= 0xF4 && pos + 3 < s.Length) {
    u8 b1 = p[1];
    u8 b2 = p[2];
    u8 b3 = p[3];
    bool valid = (b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80 && (b3 & 0xC0) == 0x80;
    if (valid && b0 == 0xF0 && b1 < 0x90) valid = false;
    if (valid && b0 == 0xF4 && b1 >= 0x90) valid = false;
    if (valid) {
      cp = ((u32)(b0 & 0x07) << 18) | ((u32)(b1 & 0x3F) << 12) | ((u32)(b2 & 0x3F) << 6) |
           (u32)(b3 & 0x3F);
      len = 4;
    }
  }

  u32 mapped = cp;
  if (cp == 0x2018 || cp == 0x2019 || cp == 0x201A || cp == 0x201B) mapped = '\'';
  if (cp == 0x201C || cp == 0x201D || cp == 0x201E || cp == 0x201F) mapped = '"';
  if ((cp >= 0x2010 && cp <= 0x2015) || cp == 0x2212) mapped = '-';
  if (cp == 0x00A0 || (cp >= 0x2002 && cp <= 0x200A) || cp == 0x202F || cp == 0x205F ||
      cp == 0x3000) {
    mapped = ' ';
  }

  r.Cp = cp;
  r.Len = len;
  r.Mapped = mapped;
  return r;
}

static bool Edit_IsHorizWs(u32 c) { return c == ' ' || c == '\t'; }

static u32 Edit_SkipHorizWs(StrView s, u32 pos) {
  while (pos < s.Length) {
    EditChar ch = Edit_DecodeMapped(s, pos);
    if (!ch.Len || !Edit_IsHorizWs(ch.Mapped)) break;
    pos += ch.Len;
  }
  return pos;
}

static bool Edit_FuzzyAt(StrView content, StrView oldText, u32 start, u32* end) {
  u32 cpos = start;
  u32 opos = 0;
  while (opos < oldText.Length) {
    if (cpos >= content.Length) return false;
    EditChar cc = Edit_DecodeMapped(content, cpos);
    EditChar oc = Edit_DecodeMapped(oldText, opos);
    if (cc.Len == 0 || oc.Len == 0) return false;

    if (Edit_IsHorizWs(cc.Mapped) && Edit_IsHorizWs(oc.Mapped)) {
      do {
        cpos += cc.Len;
        if (cpos >= content.Length) break;
        cc = Edit_DecodeMapped(content, cpos);
      } while (cc.Len && Edit_IsHorizWs(cc.Mapped));
      do {
        opos += oc.Len;
        if (opos >= oldText.Length) break;
        oc = Edit_DecodeMapped(oldText, opos);
      } while (oc.Len && Edit_IsHorizWs(oc.Mapped));
      continue;
    }

    if (Edit_IsHorizWs(cc.Mapped) && oc.Mapped == '\n') {
      u32 next = Edit_SkipHorizWs(content, cpos);
      EditChar nc = Edit_DecodeMapped(content, next);
      if (nc.Len && nc.Mapped == '\n') {
        cpos = next;
        continue;
      }
    }

    if (cc.Mapped == '\n' && Edit_IsHorizWs(oc.Mapped)) {
      u32 next = Edit_SkipHorizWs(oldText, opos);
      EditChar no = Edit_DecodeMapped(oldText, next);
      if (no.Len && no.Mapped == '\n') {
        opos = next;
        continue;
      }
    }

    if (cc.Mapped != oc.Mapped) return false;
    cpos += cc.Len;
    opos += oc.Len;
  }
  *end = cpos;
  return true;
}

static u32 Edit_FindAllFuzzy(StrView content, StrView oldText, u32* firstStart, u32* firstEnd) {
  u32 count = 0;
  u32 pos = 0;
  while (pos < content.Length) {
    u32 end = pos;
    if (Edit_FuzzyAt(content, oldText, pos, &end)) {
      if (count == 0) {
        if (firstStart) *firstStart = pos;
        if (firstEnd) *firstEnd = end;
      }
      count++;
      pos = end > pos ? end : pos + 1;
    } else {
      pos++;
    }
  }
  return count;
}

static void Edit_AppendIndexedOldTextEmpty(StrBuf* err, StrView path, u32 index, bool multi) {
  if (multi) {
    StrBuf_AppendCStr(err, "edits[");
    StrBuf_AppendU64(err, index);
    StrBuf_AppendCStr(err, "].oldText must not be empty in ");
    StrBuf_Append(err, path);
    StrBuf_AppendChar(err, '.');
  } else {
    Edit_AppendPathErr(err, "oldText must not be empty in ", path, ".");
  }
}

static void Edit_AppendNotFound(StrBuf* err, StrView path, u32 index, bool multi) {
  if (multi) {
    StrBuf_AppendCStr(err, "Could not find edits[");
    StrBuf_AppendU64(err, index);
    StrBuf_AppendCStr(err, "] in ");
    StrBuf_Append(err, path);
    StrBuf_AppendCStr(err, ". The oldText must match exactly including all whitespace and newlines.");
  } else {
    Edit_AppendPathErr(err, "Could not find the exact text in ", path,
                       ". The old text must match exactly including all whitespace and newlines.");
  }
}

static void Edit_AppendDuplicate(StrBuf* err, StrView path, u32 index, bool multi, u32 count) {
  StrBuf_AppendCStr(err, "Found ");
  StrBuf_AppendU64(err, count);
  if (multi) {
    StrBuf_AppendCStr(err, " occurrences of edits[");
    StrBuf_AppendU64(err, index);
    StrBuf_AppendCStr(err, "] in ");
    StrBuf_Append(err, path);
    StrBuf_AppendCStr(err,
                      ". Each oldText must be unique. Please provide more context to make it unique.");
  } else {
    StrBuf_AppendCStr(err, " occurrences of the text in ");
    StrBuf_Append(err, path);
    StrBuf_AppendCStr(err, ". The text must be unique. Please provide more context to make it unique.");
  }
}

static void Edit_SortMatches(Array<EditMatch>* matches) {
  for (u32 i = 1; i < matches->Count; i++) {
    EditMatch v = matches->Data[i];
    u32 j = i;
    while (j > 0 && matches->Data[j - 1].Start > v.Start) {
      matches->Data[j] = matches->Data[j - 1];
      j--;
    }
    matches->Data[j] = v;
  }
}

static StrView Edit_Apply(Arena* arena, StrView original, Array<EditSpec>* edits, Array<EditMatch>* matches) {
  StrView cur = original;
  for (u32 rev = matches->Count; rev > 0; rev--) {
    EditMatch m = matches->Data[rev - 1];
    StrView repl = edits->Data[m.Index].NewText;
    u32 newLen = cur.Length - (m.End - m.Start) + repl.Length;
    StrBuf b = StrBuf_Make(arena, newLen + 1);
    StrBuf_Append(&b, StrView{cur.Data, m.Start});
    StrBuf_Append(&b, repl);
    StrBuf_Append(&b, StrView{cur.Data + m.End, cur.Length - m.End});
    cur = StrBuf_View(&b);
  }
  return cur;
}

static JsonValue* Edit_NewValue(Arena* arena, JsonKind kind) {
  JsonValue* value = ArenaPushStruct(arena, JsonValue);
  *value = JsonValue{};
  value->Kind = kind;
  return value;
}

static bool EditPrepareArguments(void*, Arena* arena, JsonValue* input,
                                 JsonValue** out, StrBuf* err) {
  if (!Json_IsObject(input)) {
    *out = input;
    return true;
  }

  JsonValue* edits = Json_Get(input, "edits");
  if (Json_IsString(edits)) {
    JsonValue* parsed = Json_Parse(arena, edits->Str);
    if (!parsed) {
      StrBuf_AppendCStr(err, "edits contains invalid JSON");
      return false;
    }
    *edits = *parsed;
    *out = input;
    return true;
  }

  JsonValue* oldText = Json_Get(input, "oldText");
  JsonValue* newText = Json_Get(input, "newText");
  if (edits || !Json_IsString(oldText) || !Json_IsString(newText)) {
    *out = input;
    return true;
  }

  JsonValue* item = Edit_NewValue(arena, JsonKind::Object);
  item->Members = ArenaPushArray(arena, JsonMember, 2);
  item->MemberCount = 2;
  item->Members[0] = JsonMember{SVL("oldText"), oldText};
  item->Members[1] = JsonMember{SVL("newText"), newText};

  JsonValue* array = Edit_NewValue(arena, JsonKind::Array);
  array->Items = ArenaPushArray(arena, JsonValue*, 1);
  array->Items[0] = item;
  array->Count = 1;

  JsonValue* path = Json_Get(input, "path");
  JsonValue* root = Edit_NewValue(arena, JsonKind::Object);
  root->Members = ArenaPushArray(arena, JsonMember, 2);
  root->MemberCount = path ? 2u : 1u;
  u32 member = 0;
  if (path) root->Members[member++] = JsonMember{SVL("path"), path};
  root->Members[member] = JsonMember{SVL("edits"), array};
  *out = root;
  return true;
}

static bool Edit_ParseEdits(const JsonValue* j, Array<EditSpec>* edits) {
  const JsonValue* ev = Json_Get(j, "edits");
  if (!Json_IsArray(ev)) return false;

  for (u32 i = 0; i < ev->Count; i++) {
    const JsonValue* item = ev->Items[i];
    EditSpec e;
    e.OldText = Json_GetStr(item, "oldText");
    e.NewText = Json_GetStr(item, "newText");
    Array_Push(edits, e);
  }
  return true;
}

bool EditExec(void* ctx, Arena* arena, StrView id, const ToolArgs* args,
              volatile i32* abort, const ToolUpdateSink* updates,
              ToolResult* out, StrBuf* err) {
  (void)ctx;
  (void)id;
  (void)abort;
  (void)updates;
  const JsonValue* j = args->Root;
  StrView path = ToolArgs_GetStr(args, "path");
  Array<EditSpec> edits = Array_Make<EditSpec>(arena, 4);
  if (path.Length == 0 || !Edit_ParseEdits(j, &edits) || edits.Count == 0) {
    StrBuf_AppendCStr(err, "Edit tool input is invalid. edits must contain at least one replacement.");
    return false;
  }

  StrView realPath = Fs_RealPath(arena, path);
  FileLockToken lock = FileLock_Acquire(realPath);
  bool ok = false;

  StrView fileBytes = {};
  bool hasBom = false;
  StrView body = {};
  bool crlf = false;
  StrView originalLF = {};
  if (!Edit_ReadFile(arena, path, &fileBytes)) {
    Edit_AppendPathErr(err, "Could not edit file: ", path, ".");
    goto done;
  }

  hasBom = fileBytes.Length >= 3 && (u8)fileBytes.Data[0] == 0xEF && (u8)fileBytes.Data[1] == 0xBB &&
           (u8)fileBytes.Data[2] == 0xBF;
  body = fileBytes;
  if (hasBom) body = StrView{fileBytes.Data + 3, fileBytes.Length - 3};
  crlf = Edit_IsCrlf(body);
  originalLF = Edit_NormalizeLf(arena, body);

  for (u32 i = 0; i < edits.Count; i++) {
    edits.Data[i].OldText = Edit_NormalizeLf(arena, edits.Data[i].OldText);
    edits.Data[i].NewText = Edit_NormalizeLf(arena, edits.Data[i].NewText);
  }

  {
    bool multi = edits.Count > 1;
    Array<EditMatch> matches = Array_Make<EditMatch>(arena, edits.Count);
    for (u32 i = 0; i < edits.Count; i++) {
      if (edits.Data[i].OldText.Length == 0) {
        Edit_AppendIndexedOldTextEmpty(err, path, i, multi);
        goto done;
      }

      u32 start = 0;
      u32 end = 0;
      u32 count = Edit_FindAllExact(originalLF, edits.Data[i].OldText, &start);
      if (count == 1) {
        end = start + edits.Data[i].OldText.Length;
      } else if (count > 1) {
        Edit_AppendDuplicate(err, path, i, multi, count);
        goto done;
      } else {
        count = Edit_FindAllFuzzy(originalLF, edits.Data[i].OldText, &start, &end);
        if (count == 0) {
          Edit_AppendNotFound(err, path, i, multi);
          goto done;
        }
        if (count > 1) {
          Edit_AppendDuplicate(err, path, i, multi, count);
          goto done;
        }
      }

      EditMatch m;
      m.Start = start;
      m.End = end;
      m.Index = i;
      Array_Push(&matches, m);
    }

    Edit_SortMatches(&matches);
    for (u32 i = 1; i < matches.Count; i++) {
      if (matches.Data[i - 1].End > matches.Data[i].Start) {
        StrBuf_AppendCStr(err, "edits[");
        StrBuf_AppendU64(err, matches.Data[i - 1].Index);
        StrBuf_AppendCStr(err, "] and edits[");
        StrBuf_AppendU64(err, matches.Data[i].Index);
        StrBuf_AppendCStr(err, "] overlap in ");
        StrBuf_Append(err, path);
        StrBuf_AppendCStr(err, ". Merge them into one edit or target disjoint regions.");
        goto done;
      }
    }

    StrView newLF = Edit_Apply(arena, originalLF, &edits, &matches);
    if (StrView_Eq(originalLF, newLF)) {
      Edit_AppendPathErr(err, "No changes made to ", path, ". The replacement produced identical content...");
      goto done;
    }

    StrView writeBytes = crlf ? Edit_RestoreCrlf(arena, newLF) : newLF;
    if (hasBom) writeBytes = Edit_PrependBom(arena, writeBytes);
    if (!Edit_WriteFile(arena, path, writeBytes)) {
      Edit_AppendPathErr(err, "Could not edit file: ", path, ".");
      goto done;
    }

    DiffResult d = Diff_Make(arena, path, originalLF, newLF, 4);
    StrBuf result = StrBuf_Make(arena, d.Display.Length + path.Length + 96);
    StrBuf_AppendCStr(&result, "Successfully replaced ");
    StrBuf_AppendU64(&result, edits.Count);
    StrBuf_AppendCStr(&result, " block(s) in ");
    StrBuf_Append(&result, path);
    StrBuf_AppendCStr(&result, ".\n\n");
    StrBuf_Append(&result, d.Display);

    out->Content = Array_Make<Content>(arena, 1);
    Array_Push(&out->Content, Content_Text(StrBuf_View(&result)));
    ok = true;
  }

done:
  FileLock_Release(lock);
  return ok;
}

static bool EditDescribePermission(void* ctx, Arena* arena,
                                   const ToolArgs* args,
                                   StrView workingDirectory,
                                   ToolPermissionInfo* out, StrBuf* err) {
  (void)ctx;
  return Fs_DescribeMutationPermission(
      arena, args, workingDirectory, SVL("Edit file "), SVL("edit\n"), out,
      err);
}

constexpr StrView kEditPromptGuidelines[] = {
    SVL("Use edit for precise changes (edits[].oldText must match exactly)"),
    SVL("When changing multiple separate locations in one file, use one edit "
        "call with multiple entries in edits[] instead of multiple edit calls"),
    SVL("Each edits[].oldText is matched against the original file, not after "
        "earlier edits are applied. Do not emit overlapping or nested edits. "
        "Merge nearby changes into one edit."),
    SVL("Keep edits[].oldText as small as possible while still being unique in "
        "the file. Do not pad with large unchanged regions.")};

Tool EditTool() {
  Tool t = {};
  t.Name = SVL("edit");
  t.Label = SVL("Edit");
  t.Description = SVL("Replace one or more unique text blocks in a UTF-8 file.");
  t.PromptSnippet = SVL(
      "Make precise file edits with exact text replacement, including multiple "
      "disjoint edits in one call");
  Tool_SetPromptGuidelines(&t, kEditPromptGuidelines);
  t.ParamsJsonSchema = SVL(
      "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Path to the file to "
      "edit\"},\"edits\":{\"type\":\"array\",\"description\":\"Targeted replacements to apply\",\"items\":{\"type\":"
      "\"object\",\"properties\":{\"oldText\":{\"type\":\"string\",\"description\":\"Exact text for one targeted "
      "replacement; must be unique in the file and not overlap other edits\"},\"newText\":{\"type\":\"string\","
      "\"description\":\"Replacement text\"}},\"required\":[\"oldText\",\"newText\"]}}},\"required\":[\"path\","
      "\"edits\"]}");
  t.PrepareArguments = EditPrepareArguments;
  t.Execute = EditExec;
  t.ExecMode = ToolExecMode::Default;
  t.PermissionEffect = ToolPermissionEffect::Sensitive;
  t.DescribePermission = EditDescribePermission;
  return t;
}
