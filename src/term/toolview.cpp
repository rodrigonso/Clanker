#include "term/toolview.h"

#include <string.h>

#include "json/json.h"
#include "term/cell.h"
#include "term/sanitize.h"

namespace {

const u32 TOOL_LABEL_MAX_BYTES = 32;
const u32 TOOL_SUMMARY_MAX_BYTES = 240;
const u32 TOOL_ARGS_PREVIEW_BYTES = 160;
const u32 TOOL_PATH_MAX_BYTES = 300;
const u32 TOOL_FILTER_MAX_BYTES = 320;
const usize TOOL_PARSE_SCRATCH_BASE = (usize)64 * 1024;
const usize TOOL_PARSE_SCRATCH_SCALE = 16;

bool Eq(StrView s, const char* text) { return StrView_EqCStr(s, text); }

char Lower(char c) {
  if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
  return c;
}

bool EndsWithI(StrView s, const char* suffix) {
  u32 n = (u32)strlen(suffix);
  if (n > s.Length) return false;
  u32 start = s.Length - n;
  for (u32 i = 0; i < n; i++) {
    if (Lower(s.Data[start + i]) != Lower(suffix[i])) return false;
  }
  return true;
}

StrView ReadMarker(StrView path) {
  if (EndsWithI(path, ".cpp") || EndsWithI(path, ".cc") || EndsWithI(path, ".cxx")) return SVL("C+");
  if (EndsWithI(path, ".h") || EndsWithI(path, ".hpp")) return SVL("H");
  if (EndsWithI(path, ".md")) return SVL("MD");
  if (EndsWithI(path, ".json")) return SVL("J");
  if (EndsWithI(path, ".ts") || EndsWithI(path, ".tsx")) return SVL("TS");
  if (EndsWithI(path, ".js") || EndsWithI(path, ".jsx")) return SVL("JS");
  if (EndsWithI(path, ".py")) return SVL("PY");
  return SVL("R");
}

void AppendFlat(Arena* scratch, StrBuf* out, StrView text, u32 maxBytes) {
  StrBuf clean = StrBuf_Make(scratch, text.Length + 1);
  Sanitize_Display(&clean, text);
  bool pendingSpace = false;
  u32 i = 0;
  while (i < clean.Length) {
    u32 next = i;
    u32 cp = Utf8Decode(StrBuf_View(&clean), &next);
    if (cp == '\n') {
      pendingSpace = out->Length != 0;
      i = next;
      continue;
    }
    if (pendingSpace && cp != ' ') {
      if (out->Length >= maxBytes) break;
      StrBuf_AppendChar(out, ' ');
    }
    pendingSpace = false;
    u32 byteCount = next - i;
    if (out->Length + byteCount > maxBytes) break;
    StrBuf_Append(out, StrView{clean.Data + i, byteCount});
    i = next;
  }
}

void AppendQuoted(Arena* scratch, StrBuf* b, StrView text) {
  StrBuf_AppendChar(b, '"');
  AppendFlat(scratch, b, text, TOOL_SUMMARY_MAX_BYTES);
  StrBuf_AppendChar(b, '"');
}

void AppendCount(StrBuf* b, u32 count, const char* singular, const char* plural) {
  StrBuf_AppendU64(b, count);
  StrBuf_AppendChar(b, ' ');
  StrBuf_AppendCStr(b, count == 1 ? singular : plural);
}

bool IsNoticeLine(StrView line) {
  if (line.Length < 2 || line.Data[0] != '[') return false;
  return StrView_Contains(line, "limit reached") || StrView_Contains(line, "Showing lines") ||
         StrView_Contains(line, "more lines in file") || StrView_Contains(line, "exceeds");
}

StrView PayloadBeforeNotice(StrView text) {
  for (u32 i = 0; i + 2 < text.Length; i++) {
    if (text.Data[i] != '\n' || text.Data[i + 1] != '\n' || text.Data[i + 2] != '[') continue;
    u32 end = i + 2;
    while (end < text.Length && text.Data[end] != '\n') end++;
    if (IsNoticeLine(StrView{text.Data + i + 2, end - (i + 2)})) return StrView{text.Data, i};
  }
  return text;
}

u32 CountLines(StrView text, bool skipEmpty, bool skipNotice) {
  if (text.Length == 0) return 0;
  u32 count = 0;
  u32 i = 0;
  while (i < text.Length) {
    u32 start = i;
    while (i < text.Length && text.Data[i] != '\n') i++;
    u32 end = i;
    if (end > start && text.Data[end - 1] == '\r') end--;
    StrView line{text.Data + start, end - start};
    if ((!skipEmpty || line.Length != 0) && (!skipNotice || !IsNoticeLine(line))) count++;
    if (i >= text.Length) break;
    i++;
  }
  return count;
}

bool IsGrepMatchLine(StrView line) {
  for (u32 i = 0; i + 2 < line.Length; i++) {
    if (line.Data[i] != ':' || line.Data[i + 1] < '0' || line.Data[i + 1] > '9') continue;
    u32 j = i + 1;
    while (j < line.Length && line.Data[j] >= '0' && line.Data[j] <= '9') j++;
    if (j < line.Length && line.Data[j] == ':') return true;
  }
  return false;
}

// Edit results embed a Diff_Make display where every line is
// "<prefix><lineNo><pad> <text>" and prefix is '+', '-' or ' '. Counting lines
// whose prefix is '+'/'-' followed by a digit yields the added/removed totals
// without re-diffing. The digit check keeps prose lines (e.g. a body line that
// happens to start with '-') from being miscounted.
void CountDiffLines(StrView text, u32* added, u32* removed) {
  *added = 0;
  *removed = 0;
  u32 i = 0;
  while (i < text.Length) {
    u32 start = i;
    while (i < text.Length && text.Data[i] != '\n') i++;
    u32 end = i;
    if (end > start && text.Data[end - 1] == '\r') end--;
    if (end - start >= 2) {
      char prefix = text.Data[start];
      char next = text.Data[start + 1];
      if (next >= '0' && next <= '9') {
        if (prefix == '+') (*added)++;
        else if (prefix == '-') (*removed)++;
      }
    }
    if (i >= text.Length) break;
    i++;
  }
}

u32 CountGrepMatches(StrView text) {
  u32 count = 0;
  u32 i = 0;
  while (i < text.Length) {
    u32 start = i;
    while (i < text.Length && text.Data[i] != '\n') i++;
    u32 end = i;
    if (end > start && text.Data[end - 1] == '\r') end--;
    if (IsGrepMatchLine(StrView{text.Data + start, end - start})) count++;
    if (i < text.Length) i++;
  }
  return count;
}

void InitUnknown(Arena* arena, Arena* scratch, ToolView* view, StrView name, StrView args) {
  view->Kind = ToolViewKind::Unknown;
  view->Marker = SVL(">");
  StrBuf label = StrBuf_Make(scratch, TOOL_LABEL_MAX_BYTES + 1);
  AppendFlat(scratch, &label, name, TOOL_LABEL_MAX_BYTES);
  view->Label = StrView_Dup(arena, StrBuf_View(&label));
  u32 n = args.Length < TOOL_ARGS_PREVIEW_BYTES ? args.Length : TOOL_ARGS_PREVIEW_BYTES;
  while (n < args.Length && n > 0 && ((u8)args.Data[n] & 0xC0) == 0x80) n--;
  AppendFlat(scratch, &view->Detail, StrView{args.Data, n}, TOOL_SUMMARY_MAX_BYTES);
  if (n < args.Length) StrBuf_AppendCStr(&view->Detail, "...");
}

void InitParsed(Arena* arena, Arena* scratch, ToolView* view, StrView name, StrView argsJson) {
  *view = {};
  view->Detail = StrBuf_Make(arena, 128);
  view->Outcome = StrBuf_Make(arena, 48);

  StrBuf jsonText = StrBuf_Make(scratch, argsJson.Length + 1);
  StrBuf_Append(&jsonText, argsJson);
  JsonValue* args = Json_Parse(scratch, StrBuf_View(&jsonText));
  if (!Json_IsObject(args)) {
    InitUnknown(arena, scratch, view, name, argsJson);
    return;
  }

  if (Eq(name, "shell")) {
    view->Kind = ToolViewKind::Shell;
    view->Marker = SVL("$");
    view->Label = SVL("Shell");
    AppendFlat(scratch, &view->Detail, Json_GetStr(args, "command"), TOOL_SUMMARY_MAX_BYTES);
    return;
  }

  if (Eq(name, "read")) {
    view->Kind = ToolViewKind::Read;
    StrView path = Json_GetStr(args, "path");
    view->Marker = ReadMarker(path);
    view->Label = SVL("Read");
    i64 offset = Json_GetInt(args, "offset", 0);
    i64 limit = Json_GetInt(args, "limit", 0);
    AppendFlat(scratch, &view->Detail, path, TOOL_SUMMARY_MAX_BYTES);
    if (offset > 0) {
      StrBuf_AppendCStr(&view->Detail, " L");
      StrBuf_AppendI64(&view->Detail, offset);
      if (limit > 0) {
        StrBuf_AppendChar(&view->Detail, ':');
        StrBuf_AppendI64(&view->Detail, offset + limit - 1);
      }
    }
    return;
  }

  if (Eq(name, "grep")) {
    view->Kind = ToolViewKind::Grep;
    view->Marker = SVL("/");
    view->Label = SVL("Search");
    AppendQuoted(scratch, &view->Detail, Json_GetStr(args, "pattern"));
    StrBuf_AppendCStr(&view->Detail, " in ");
    StrView path = Json_GetStr(args, "path");
    AppendFlat(scratch, &view->Detail, path.Length ? path : SVL("."), TOOL_PATH_MAX_BYTES);
    StrView glob = Json_GetStr(args, "glob");
    if (glob.Length) {
      StrBuf_AppendCStr(&view->Detail, " (");
      AppendFlat(scratch, &view->Detail, glob, TOOL_FILTER_MAX_BYTES);
      StrBuf_AppendChar(&view->Detail, ')');
    }
    return;
  }

  if (Eq(name, "find")) {
    view->Kind = ToolViewKind::Find;
    view->Marker = SVL("/");
    view->Label = SVL("Find");
    AppendQuoted(scratch, &view->Detail, Json_GetStr(args, "pattern"));
    StrBuf_AppendCStr(&view->Detail, " in ");
    StrView path = Json_GetStr(args, "path");
    AppendFlat(scratch, &view->Detail, path.Length ? path : SVL("."), TOOL_PATH_MAX_BYTES);
    return;
  }

  if (Eq(name, "ls")) {
    view->Kind = ToolViewKind::Ls;
    view->Marker = SVL("L");
    view->Label = SVL("List");
    StrView path = Json_GetStr(args, "path");
    AppendFlat(scratch, &view->Detail, path.Length ? path : SVL("."),
               TOOL_SUMMARY_MAX_BYTES);
    return;
  }

  if (Eq(name, "write")) {
    view->Kind = ToolViewKind::Write;
    view->Marker = SVL("+");
    view->Label = SVL("Write");
    AppendFlat(scratch, &view->Detail, Json_GetStr(args, "path"), TOOL_SUMMARY_MAX_BYTES);
    view->CountHint = Json_GetStr(args, "content").Length;
    return;
  }

  if (Eq(name, "edit")) {
    view->Kind = ToolViewKind::Edit;
    view->Marker = SVL("~");
    view->Label = SVL("Edit");
    AppendFlat(scratch, &view->Detail, Json_GetStr(args, "path"), TOOL_SUMMARY_MAX_BYTES);
    JsonValue* edits = Json_Get(args, "edits");
    if (Json_IsString(edits)) {
      StrBuf nested = StrBuf_Make(scratch, edits->Str.Length + 1);
      StrBuf_Append(&nested, edits->Str);
      edits = Json_Parse(scratch, StrBuf_View(&nested));
    }
    if (Json_IsArray(edits)) {
      view->CountHint = edits->Count;
    } else if (Json_IsString(Json_Get(args, "oldText")) &&
               Json_IsString(Json_Get(args, "newText"))) {
      view->CountHint = 1;
    }
    return;
  }

  InitUnknown(arena, scratch, view, name, argsJson);
}

}  // namespace

void ToolView_Init(Arena* arena, ToolView* view, StrView name, StrView argsJson) {
  usize reserve = (usize)argsJson.Length * TOOL_PARSE_SCRATCH_SCALE + TOOL_PARSE_SCRATCH_BASE;
  Arena scratch = ArenaCreate(reserve);
  InitParsed(arena, &scratch, view, name, argsJson);
  ArenaRelease(&scratch);
}

void ToolView_Finish(ToolView* view, StrView result, bool isError) {
  StrBuf_Clear(&view->Outcome);
  view->OutcomeSplit = 0;
  if (isError) {
    StrBuf_AppendCStr(&view->Outcome, "failed");
    return;
  }

  switch (view->Kind) {
    case ToolViewKind::Shell:
      break;
    case ToolViewKind::Read: {
      if (StrView_Contains(result, "Read image file [")) {
        StrBuf_AppendCStr(&view->Outcome, "image read");
        break;
      }
      u32 count = CountLines(PayloadBeforeNotice(result), false, false);
      AppendCount(&view->Outcome, count, "line read", "lines read");
    } break;
    case ToolViewKind::Grep: {
      if (StrView_Contains(result, "No matches found")) {
        StrBuf_AppendCStr(&view->Outcome, "no matches");
        break;
      }
      u32 count = CountGrepMatches(result);
      AppendCount(&view->Outcome, count, "line found", "lines found");
    } break;
    case ToolViewKind::Find: {
      if (StrView_Contains(result, "No files found matching pattern")) {
        StrBuf_AppendCStr(&view->Outcome, "no files found");
        break;
      }
      u32 count = CountLines(result, true, true);
      AppendCount(&view->Outcome, count, "file found", "files found");
    } break;
    case ToolViewKind::Ls: {
      if (StrView_Contains(result, "(empty directory)")) {
        StrBuf_AppendCStr(&view->Outcome, "empty");
        break;
      }
      u32 count = CountLines(result, true, true);
      AppendCount(&view->Outcome, count, "entry found", "entries found");
    } break;
    case ToolViewKind::Write:
      AppendCount(&view->Outcome, view->CountHint, "byte written", "bytes written");
      break;
    case ToolViewKind::Edit: {
      u32 added = 0;
      u32 removed = 0;
      CountDiffLines(result, &added, &removed);
      if (added == 0 && removed == 0) {
        // No parsable diff (unexpected result shape); fall back to block count.
        AppendCount(&view->Outcome, view->CountHint, "block replaced", "blocks replaced");
        break;
      }
      StrBuf_AppendChar(&view->Outcome, '+');
      StrBuf_AppendU64(&view->Outcome, added);
      StrBuf_AppendChar(&view->Outcome, ' ');
      // Everything from here on is the removal half; the renderer colours it red.
      view->OutcomeSplit = view->Outcome.Length;
      StrBuf_AppendChar(&view->Outcome, '-');
      StrBuf_AppendU64(&view->Outcome, removed);
    } break;
    case ToolViewKind::Unknown:
      StrBuf_AppendCStr(&view->Outcome, "completed");
      break;
  }
}
