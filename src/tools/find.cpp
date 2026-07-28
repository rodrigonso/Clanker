#include "json/json.h"
#include "tools/fs.h"
#include "tools/glob.h"
#include "tools/tools.h"
#include "tools/truncate.h"

static bool Find_StartsWith(StrView s, StrView prefix) {
  return s.Length >= prefix.Length &&
         (prefix.Length == 0 || memcmp(s.Data, prefix.Data, prefix.Length) == 0);
}

static StrView Find_AdjustPattern(Arena* arena, StrView pattern, bool* matchPath) {
  bool hasSlash = false;
  for (u32 i = 0; i < pattern.Length; i++) {
    if (pattern.Data[i] == '/') {
      hasSlash = true;
      break;
    }
  }

  *matchPath = hasSlash;
  if (!hasSlash) return pattern;
  if (Find_StartsWith(pattern, SVL("/")) || Find_StartsWith(pattern, SVL("**/")) ||
      StrView_Eq(pattern, SVL("**"))) {
    return pattern;
  }

  StrBuf b = StrBuf_Make(arena, pattern.Length + 4);
  StrBuf_AppendCStr(&b, "**/");
  StrBuf_Append(&b, pattern);
  return StrBuf_View(&b);
}

struct FindState {
  Arena* Arena;
  Array<StrView> Results;
  StrView Pattern;
  u32 Limit;
  bool MatchPath;
  bool HitLimit;
};

static bool Find_Visit(void* user, StrView absPath, StrView relPath, const FsEntry& e) {
  (void)absPath;
  FindState* st = (FindState*)user;
  StrView candidate = st->MatchPath ? relPath : e.Name;
  if (!Glob_Match(st->Pattern, candidate, false)) return true;

  StrView result;
  if (e.IsDir) {
    StrBuf b = StrBuf_Make(st->Arena, relPath.Length + 2);
    StrBuf_Append(&b, relPath);
    StrBuf_AppendChar(&b, '/');
    result = StrBuf_View(&b);
  } else {
    result = StrView_Dup(st->Arena, relPath);
  }
  Array_Push(&st->Results, result);
  if (st->Results.Count >= st->Limit) {
    st->HitLimit = true;
    return false;
  }
  return true;
}

static bool FindExec(void* ctx, Arena* arena, StrView id, StrView argsJson, volatile i32* abort,
                     ToolResult* out, StrBuf* err) {
  (void)ctx;
  (void)id;
  (void)abort;

  JsonValue* j = Json_Parse(arena, argsJson);
  StrView pattern = Json_GetStr(j, "pattern");
  if (pattern.Length == 0) {
    StrBuf_AppendCStr(err, "missing 'pattern' argument");
    return false;
  }

  StrView path = Json_GetStr(j, "path");
  if (path.Length == 0) path = SVL(".");

  i64 limitArg = Json_GetInt(j, "limit", 1000);
  u32 limit = 1000;
  if (limitArg > 0) {
    limit = limitArg > (i64)0xFFFFFFFFu ? 0xFFFFFFFFu : (u32)limitArg;
  }

  if (!Fs_Exists(path)) {
    StrBuf_AppendCStr(err, "Path not found: ");
    StrBuf_Append(err, path);
    return false;
  }

  bool matchPath = false;
  StrView effPattern = Find_AdjustPattern(arena, pattern, &matchPath);

  FindState st = {};
  st.Arena = arena;
  st.Results = Array_Make<StrView>(arena, limit < 64u ? limit : 64u);
  st.Pattern = effPattern;
  st.Limit = limit;
  st.MatchPath = matchPath;

  FsWalkOptions opts = {};
  opts.RespectGitignore = true;
  opts.IncludeDirs = true;
  Fs_Walk(arena, path, opts, Find_Visit, &st);

  StrBuf raw = StrBuf_Make(arena, st.Results.Count ? st.Results.Count * 32u : 64u);
  if (st.Results.Count == 0) {
    StrBuf_AppendCStr(&raw, "No files found matching pattern");
  } else {
    for (u32 i = 0; i < st.Results.Count; i++) {
      if (i) StrBuf_AppendChar(&raw, '\n');
      StrBuf_Append(&raw, st.Results.Data[i]);
    }
  }

  TruncResult tr = Trunc_Head(arena, StrBuf_View(&raw), 0xFFFFFFFFu, TRUNC_MAX_BYTES);
  StrBuf text = StrBuf_Make(arena, tr.Content.Length + 160);
  StrBuf_Append(&text, tr.Content);
  if (st.HitLimit || tr.Truncated) {
    if (text.Length > 0) StrBuf_AppendCStr(&text, "\n");
    StrBuf_AppendChar(&text, '[');
    if (st.HitLimit) {
      StrBuf_AppendU64(&text, st.Limit);
      StrBuf_AppendCStr(&text, " results limit reached. Use limit=");
      StrBuf_AppendU64(&text, (u64)st.Limit * 2u);
      StrBuf_AppendCStr(&text, " for more, or refine pattern.");
      if (tr.Truncated) StrBuf_AppendChar(&text, ' ');
    }
    if (tr.Truncated) StrBuf_AppendCStr(&text, "50.0KB limit reached");
    StrBuf_AppendChar(&text, ']');
  }

  out->Content = Array_Make<Content>(arena, 1);
  Array_Push(&out->Content, Content_Text(StrBuf_View(&text)));
  return true;
}

Tool FindTool() {
  Tool t = {};
  t.Name = SVL("find");
  t.Label = SVL("Find");
  t.Description = SVL("Find files and directories by glob pattern.");
  t.ParamsJsonSchema = SVL(
      "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\",\"description\":\"Glob "
      "pattern to match files, e.g. '*.ts', '**/*.json', or 'src/**/*.spec.ts'\"},\"path\":{"
      "\"type\":\"string\",\"description\":\"Directory to search in (default: current directory)\"},"
      "\"limit\":{\"type\":\"number\",\"description\":\"Maximum number of results (default: 1000)\"}},"
      "\"required\":[\"pattern\"]}");
  t.ReplayPolicy = ToolReplayPolicy::ReplaySafe;
  t.Execute = FindExec;
  return t;
}
