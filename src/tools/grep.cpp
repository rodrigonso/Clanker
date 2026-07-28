#include <stdio.h>
#include <string.h>

#include "json/json.h"
#include "tools/fs.h"
#include "tools/glob.h"
#include "tools/regex.h"
#include "tools/tools.h"
#include "tools/truncate.h"

static const u32 GREP_MAX_LINE_LENGTH = 500;

struct GrepState {
  Arena* Arena;
  StrBuf* Output;
  StrBuf* Err;
  const Regex* Re;
  StrView Pattern;
  StrView Glob;
  volatile i32* Abort;
  bool Literal;
  bool IgnoreCase;
  bool HasGlob;
  bool AnyLineTruncated;
  bool LimitHit;
  bool Aborted;
  u32 Context;
  u32 Limit;
  u32 MatchCount;
};

static char Grep_LowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

static bool Grep_LiteralSearch(StrView hay, StrView needle, bool ignoreCase) {
  if (needle.Length == 0) return true;
  if (needle.Length > hay.Length) return false;
  for (u32 i = 0; i + needle.Length <= hay.Length; i++) {
    bool same = true;
    for (u32 j = 0; j < needle.Length; j++) {
      char a = hay.Data[i + j];
      char b = needle.Data[j];
      if (ignoreCase) {
        a = Grep_LowerAscii(a);
        b = Grep_LowerAscii(b);
      }
      if (a != b) {
        same = false;
        break;
      }
    }
    if (same) return true;
  }
  return false;
}

static bool Grep_MatchesLine(const GrepState* s, StrView line) {
  if (s->Literal) return Grep_LiteralSearch(line, s->Pattern, s->IgnoreCase);
  return Regex_Test(s->Re, line);
}

static bool Grep_IsBinary(StrView bytes) {
  u32 n = ClankMin(bytes.Length, 8192u);
  for (u32 i = 0; i < n; i++) {
    if (bytes.Data[i] == '\0') return true;
  }
  return false;
}

static Array<StrView> Grep_SplitLines(Arena* arena, StrView bytes) {
  Array<StrView> lines = Array_Make<StrView>(arena, 128);
  u32 start = 0;
  for (u32 i = 0; i <= bytes.Length; i++) {
    if (i == bytes.Length || bytes.Data[i] == '\n') {
      if (i != bytes.Length || start < bytes.Length) {
        u32 end = i;
        if (end > start && bytes.Data[end - 1] == '\r') end--;
        Array_Push(&lines, StrView{bytes.Data + start, end - start});
      }
      start = i + 1;
    }
  }
  return lines;
}

static bool Grep_LineNumberInMatches(const Array<u32>* matches, u32 lineNo) {
  for (u32 i = 0; i < matches->Count; i++) {
    if (matches->Data[i] == lineNo) return true;
    if (matches->Data[i] > lineNo) return false;
  }
  return false;
}

static void Grep_AppendOutputLine(GrepState* s, StrView display, u32 lineNo, bool isMatch,
                                  StrView line) {
  StrBuf_Append(s->Output, display);
  StrBuf_AppendChar(s->Output, isMatch ? ':' : '-');
  StrBuf_AppendU64(s->Output, lineNo);
  StrBuf_AppendChar(s->Output, isMatch ? ':' : '-');
  StrBuf_AppendChar(s->Output, ' ');
  if (line.Length > GREP_MAX_LINE_LENGTH) {
    StrBuf_Append(s->Output, StrView{line.Data, GREP_MAX_LINE_LENGTH});
    StrBuf_AppendCStr(s->Output, "... [truncated]");
    s->AnyLineTruncated = true;
  } else {
    StrBuf_Append(s->Output, line);
  }
  StrBuf_AppendChar(s->Output, '\n');
}

static void Grep_AppendMatchesForFile(GrepState* s, StrView display, const Array<StrView>* lines,
                                      const Array<u32>* matches) {
  u32 lastEmitted = 0;
  for (u32 i = 0; i < matches->Count; i++) {
    u32 matchLine = matches->Data[i];
    u32 first = matchLine > s->Context ? matchLine - s->Context : 1u;
    u32 last = ClankMin(lines->Count, matchLine + s->Context);
    for (u32 lineNo = first; lineNo <= last; lineNo++) {
      if (lineNo <= lastEmitted) continue;
      bool isMatch = Grep_LineNumberInMatches(matches, lineNo);
      Grep_AppendOutputLine(s, display, lineNo, isMatch, lines->Data[lineNo - 1u]);
      lastEmitted = lineNo;
    }
  }
}

static bool Grep_SearchFile(GrepState* s, StrView path, StrView display, bool failOnOpen) {
  if (s->Abort && *s->Abort) {
    s->Aborted = true;
    return false;
  }
  if (s->HasGlob && !Glob_Match(s->Glob, display, false)) return true;

  StrBuf p = StrBuf_Make(s->Arena, path.Length + 1);
  StrBuf_Append(&p, path);
  FILE* f = fopen(StrBuf_CStr(&p), "rb");
  if (!f) {
    if (failOnOpen) {
      StrBuf_AppendCStr(s->Err, "cannot open file: ");
      StrBuf_Append(s->Err, path);
      return false;
    }
    return true;
  }

  StrBuf file = StrBuf_Make(s->Arena, 8192);
  char buf[8192];
  size_t n = 0;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    StrBuf_Append(&file, StrView{buf, (u32)n});
    if (s->Abort && *s->Abort) {
      s->Aborted = true;
      fclose(f);
      return false;
    }
  }
  fclose(f);

  StrView bytes = StrBuf_View(&file);
  if (Grep_IsBinary(bytes)) return true;

  Array<StrView> lines = Grep_SplitLines(s->Arena, bytes);
  Array<u32> matches = Array_Make<u32>(s->Arena, 8);
  for (u32 i = 0; i < lines.Count; i++) {
    if (Grep_MatchesLine(s, lines.Data[i])) {
      Array_Push(&matches, i + 1u);
      s->MatchCount++;
      if (s->MatchCount >= s->Limit) {
        s->LimitHit = true;
        break;
      }
    }
  }

  Grep_AppendMatchesForFile(s, display, &lines, &matches);
  return !s->LimitHit;
}

static bool Grep_WalkVisit(void* user, StrView absPath, StrView relPath, const FsEntry& e) {
  (void)e;
  GrepState* s = (GrepState*)user;
  return Grep_SearchFile(s, absPath, relPath, false);
}

static void Grep_AppendNotice(StrBuf* b, const GrepState* s, bool bytesTruncated) {
  if (!s->LimitHit && !s->AnyLineTruncated && !bytesTruncated) return;
  if (b->Length > 0) StrBuf_AppendChar(b, '\n');
  StrBuf_AppendChar(b, '[');
  bool wrote = false;
  if (s->LimitHit) {
    StrBuf_AppendU64(b, s->Limit);
    StrBuf_AppendCStr(b, " matches limit reached. Use limit=");
    StrBuf_AppendU64(b, (u64)s->Limit * 2u);
    StrBuf_AppendCStr(b, " for more, or refine pattern.");
    wrote = true;
  }
  if (s->AnyLineTruncated) {
    if (wrote) StrBuf_AppendChar(b, ' ');
    StrBuf_AppendCStr(b, "Some lines truncated to 500 chars. Use read tool to see full lines");
    wrote = true;
  }
  if (bytesTruncated) {
    if (wrote) StrBuf_AppendChar(b, ' ');
    StrBuf_AppendCStr(b, "50.0KB limit reached");
  }
  StrBuf_AppendChar(b, ']');
}

static bool GrepExec(void* ctx, Arena* arena, StrView id, StrView argsJson, volatile i32* abort,
                     ToolResult* out, StrBuf* err) {
  (void)ctx;
  (void)id;
  JsonValue* j = Json_Parse(arena, argsJson);
  StrView pattern = Json_GetStr(j, "pattern");
  if (pattern.Length == 0) {
    StrBuf_AppendCStr(err, "missing 'pattern' argument");
    return false;
  }

  StrView path = Json_GetStr(j, "path");
  if (path.Length == 0) path = SVL(".");
  StrView glob = Json_GetStr(j, "glob");
  bool ignoreCase = Json_GetBool(j, "ignoreCase", false);
  bool literal = Json_GetBool(j, "literal", false);
  i64 contextArg = Json_GetInt(j, "context", 0);
  i64 limitArg = Json_GetInt(j, "limit", 100);
  u32 contextLines = contextArg > 0 ? (u32)contextArg : 0u;
  u32 limit = limitArg > 0 ? (u32)limitArg : 100u;

  Regex* re = nullptr;
  if (!literal) {
    StrBuf reErr = StrBuf_Make(arena, 128);
    re = Regex_Compile(arena, pattern, ignoreCase, &reErr);
    if (!re) {
      StrBuf_Append(err, StrBuf_View(&reErr));
      return false;
    }
  }

  StrBuf raw = StrBuf_Make(arena, 4096);
  GrepState state = {};
  state.Arena = arena;
  state.Output = &raw;
  state.Err = err;
  state.Re = re;
  state.Pattern = pattern;
  state.Glob = glob;
  state.Abort = abort;
  state.Literal = literal;
  state.IgnoreCase = ignoreCase;
  state.HasGlob = glob.Length != 0;
  state.Context = contextLines;
  state.Limit = limit;

  if (Fs_Exists(path) && !Fs_IsDir(path)) {
    StrView display = Fs_Base(path);
    if (!Grep_SearchFile(&state, path, display, true)) {
      if (state.Aborted) StrBuf_AppendCStr(err, "aborted");
      return false;
    }
  } else {
    FsWalkOptions opts = {};
    opts.RespectGitignore = true;
    opts.IncludeDirs = false;
    Fs_Walk(arena, path, opts, Grep_WalkVisit, &state);
    if (state.Aborted) {
      StrBuf_AppendCStr(err, "aborted");
      return false;
    }
  }

  if (state.MatchCount == 0) StrBuf_AppendCStr(&raw, "No matches found");
  StrView rawView = StrBuf_View(&raw);
  TruncResult tr = Trunc_Head(arena, rawView, 0xFFFFFFFFu, TRUNC_MAX_BYTES);
  StrBuf result = StrBuf_Make(arena, tr.Content.Length + 512);
  StrBuf_Append(&result, tr.Content);
  Grep_AppendNotice(&result, &state, tr.Truncated && tr.ByBytes);

  out->Content = Array_Make<Content>(arena, 1);
  Array_Push(&out->Content, Content_Text(StrBuf_View(&result)));
  return true;
}

Tool GrepTool() {
  Tool t = {};
  t.Name = SVL("grep");
  t.Label = SVL("Grep");
  t.Description = SVL("Search files with a regex or literal pattern.");
  t.ParamsJsonSchema = SVL(
      "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\",\"description\":\"Search "
      "pattern (regex or literal string)\"},\"path\":{\"type\":\"string\",\"description\":\"Directory "
      "or file to search (default: current directory)\"},\"glob\":{\"type\":\"string\",\"description\":"
      "\"Filter files by glob pattern, e.g. '*.ts'\"},\"ignoreCase\":{\"type\":\"boolean\",\"default\":"
      "false},\"literal\":{\"type\":\"boolean\",\"default\":false,\"description\":\"Treat pattern as "
      "literal string instead of regex\"},\"context\":{\"type\":\"number\",\"default\":0,\"description\":"
      "\"Lines of context before and after each match\"},\"limit\":{\"type\":\"number\",\"default\":100,"
      "\"description\":\"Maximum number of matches\"}},\"required\":[\"pattern\"]}");
  t.ReplayPolicy = ToolReplayPolicy::ReplaySafe;
  t.Execute = GrepExec;
  return t;
}
