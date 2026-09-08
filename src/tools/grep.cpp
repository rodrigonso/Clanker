#include <string.h>

#include "base/file_io.h"
#include "base/thread.h"
#include "json/json.h"
#include "tools/exec.h"
#include "tools/fs.h"
#include "tools/tools.h"
#include "tools/tools_manager.h"
#include "tools/truncate.h"

static const u32 GREP_DEFAULT_LIMIT = 100;
static const u32 GREP_MAX_LINE_LENGTH = 500;

struct GrepMatch {
  StrView FilePath;
  StrView LineText;
  u32 LineNumber;
  bool HasLineText;
};

struct GrepCapture {
  Arena* Mem;
  Array<GrepMatch> Matches;
  StrBuf Pending;
  StrBuf Stderr;
  u32 Limit;
  u32 MatchCount;
  bool LimitHit;
};

struct GrepFileCacheEntry {
  StrView Path;
  Array<StrView> Lines;
  bool ReadOk;
};

struct GrepFormatter {
  Arena* Mem;
  StrBuf* Output;
  Array<GrepFileCacheEntry> Cache;
  StrView SearchPath;
  bool SearchIsDirectory;
  bool LinesTruncated;
  u32 Context;
};

static StrView Grep_Trim(StrView text) {
  u32 start = 0;
  u32 end = text.Length;
  while (start < end &&
         (text.Data[start] == ' ' || text.Data[start] == '\t' || text.Data[start] == '\r' ||
          text.Data[start] == '\n'))
    start++;
  while (end > start &&
         (text.Data[end - 1] == ' ' || text.Data[end - 1] == '\t' ||
          text.Data[end - 1] == '\r' || text.Data[end - 1] == '\n'))
    end--;
  return StrView{text.Data + start, end - start};
}

static bool Grep_ProcessJsonLine(GrepCapture* capture, StrView line) {
  if (line.Length == 0) return true;
  JsonValue* event = Json_ParseCopy(capture->Mem, line);
  if (!event || !StrView_EqCStr(Json_GetStr(event, "type"), "match")) return true;

  JsonValue* data = Json_Get(event, "data");
  JsonValue* path = Json_Get(data, "path");
  JsonValue* lineData = Json_Get(data, "lines");
  JsonValue* lineNumber = Json_Get(data, "line_number");
  StrView filePath = Json_GetStr(path, "text");
  if (filePath.Length == 0 || !lineNumber || lineNumber->Kind != JsonKind::Number ||
      lineNumber->Number < 1 || lineNumber->Number > 4294967295.0)
    return true;

  GrepMatch match = {};
  match.FilePath = filePath;
  match.LineNumber = (u32)lineNumber->Number;
  JsonValue* lineText = Json_Get(lineData, "text");
  if (Json_IsString(lineText)) {
    match.LineText = lineText->Str;
    match.HasLineText = true;
  }
  Array_Push(&capture->Matches, match);
  capture->MatchCount++;
  if (capture->MatchCount >= capture->Limit) {
    capture->LimitHit = true;
    return false;
  }
  return true;
}

static bool Grep_OnJsonLine(void* user, StrView line) {
  return Grep_ProcessJsonLine((GrepCapture*)user, line);
}

static bool Grep_OnStdout(void* user, StrView chunk) {
  GrepCapture* capture = (GrepCapture*)user;
  if (capture->LimitHit) return false;
  StrBuf_Append(&capture->Pending, chunk);
  return StrBuf_DrainLines(&capture->Pending, capture, Grep_OnJsonLine);
}

static bool Grep_OnStderr(void* user, StrView chunk) {
  GrepCapture* capture = (GrepCapture*)user;
  StrBuf_Append(&capture->Stderr, chunk);
  return true;
}

static bool Grep_IsParentPath(StrView path) {
  return StrView_EqCStr(path, "..") ||
         (path.Length >= 3 && path.Data[0] == '.' && path.Data[1] == '.' &&
          path.Data[2] == '/');
}

static StrView Grep_FormatPath(GrepFormatter* formatter, StrView filePath) {
  if (!formatter->SearchIsDirectory) return Fs_Base(filePath);
  StrView relative = Fs_Relativize(formatter->Mem, formatter->SearchPath, filePath);
  if (relative.Length && !Grep_IsParentPath(relative)) return relative;
  return Fs_ToPosix(formatter->Mem, filePath);
}

static Array<StrView> Grep_NormalizeAndSplit(Arena* arena, StrView content) {
  StrView normalized = StrView_NormalizeLf(arena, content);

  Array<StrView> lines = Array_Make<StrView>(arena, 64);
  u32 start = 0;
  for (u32 i = 0; i <= normalized.Length; i++) {
    if (i == normalized.Length || normalized.Data[i] == '\n') {
      Array_Push(&lines, StrView{normalized.Data + start, i - start});
      start = i + 1u;
    }
  }
  return lines;
}

static GrepFileCacheEntry* Grep_GetFile(GrepFormatter* formatter, StrView path) {
  for (u32 i = 0; i < formatter->Cache.Count; i++) {
    if (StrView_Eq(formatter->Cache.Data[i].Path, path)) return &formatter->Cache.Data[i];
  }

  GrepFileCacheEntry entry = {};
  entry.Path = path;
  StrView content = {};
  entry.ReadOk = File_ReadAll(formatter->Mem, path, &content) == FileIoResult::Success;
  if (entry.ReadOk) entry.Lines = Grep_NormalizeAndSplit(formatter->Mem, content);
  Array_Push(&formatter->Cache, entry);
  return &formatter->Cache.Data[formatter->Cache.Count - 1u];
}

static u32 Grep_Utf8PrefixBytes(StrView text, u32 maxChars) {
  u32 bytes = 0;
  u32 chars = 0;
  while (bytes < text.Length && chars < maxChars) {
    u8 lead = (u8)text.Data[bytes];
    u32 width = 1;
    if ((lead & 0xE0u) == 0xC0u)
      width = 2;
    else if ((lead & 0xF0u) == 0xE0u)
      width = 3;
    else if ((lead & 0xF8u) == 0xF0u)
      width = 4;
    if (bytes + width > text.Length) width = 1;
    for (u32 i = 1; i < width; i++) {
      if (((u8)text.Data[bytes + i] & 0xC0u) != 0x80u) {
        width = 1;
        break;
      }
    }
    bytes += width;
    chars++;
  }
  return bytes;
}

static void Grep_AppendLine(GrepFormatter* formatter, StrView display, u32 lineNumber,
                            bool isMatch, StrView line) {
  if (formatter->Output->Length) StrBuf_AppendChar(formatter->Output, '\n');
  StrBuf_Append(formatter->Output, display);
  StrBuf_AppendChar(formatter->Output, isMatch ? ':' : '-');
  StrBuf_AppendU64(formatter->Output, lineNumber);
  StrBuf_AppendChar(formatter->Output, isMatch ? ':' : '-');
  StrBuf_AppendChar(formatter->Output, ' ');
  u32 prefixBytes = Grep_Utf8PrefixBytes(line, GREP_MAX_LINE_LENGTH);
  if (prefixBytes < line.Length) {
    StrBuf_Append(formatter->Output, StrView{line.Data, prefixBytes});
    StrBuf_AppendCStr(formatter->Output, "... [truncated]");
    formatter->LinesTruncated = true;
  } else {
    StrBuf_Append(formatter->Output, line);
  }
}

static StrView Grep_SanitizeMatchLine(Arena* arena, StrView line) {
  StrBuf sanitized = StrBuf_Make(arena, line.Length + 1);
  for (u32 i = 0; i < line.Length; i++) {
    if (line.Data[i] != '\r') StrBuf_AppendChar(&sanitized, line.Data[i]);
  }
  if (sanitized.Length && sanitized.Data[sanitized.Length - 1u] == '\n') {
    sanitized.Length--;
    sanitized.Data[sanitized.Length] = '\0';
  }
  return StrBuf_View(&sanitized);
}

static void Grep_FormatMatch(GrepFormatter* formatter, const GrepMatch& match) {
  StrView display = Grep_FormatPath(formatter, match.FilePath);
  if (formatter->Context == 0 && match.HasLineText) {
    Grep_AppendLine(formatter, display, match.LineNumber, true,
                    Grep_SanitizeMatchLine(formatter->Mem, match.LineText));
    return;
  }

  GrepFileCacheEntry* file = Grep_GetFile(formatter, match.FilePath);
  if (!file->ReadOk) {
    if (formatter->Output->Length) StrBuf_AppendChar(formatter->Output, '\n');
    StrBuf_Append(formatter->Output, display);
    StrBuf_AppendChar(formatter->Output, ':');
    StrBuf_AppendU64(formatter->Output, match.LineNumber);
    StrBuf_AppendCStr(formatter->Output, ": (unable to read file)");
    return;
  }

  u32 first =
      match.LineNumber > formatter->Context ? match.LineNumber - formatter->Context : 1u;
  u64 last64 = (u64)match.LineNumber + formatter->Context;
  u32 last = last64 < file->Lines.Count ? (u32)last64 : file->Lines.Count;
  for (u32 lineNumber = first; lineNumber <= last; lineNumber++) {
    Grep_AppendLine(formatter, display, lineNumber, lineNumber == match.LineNumber,
                    file->Lines.Data[lineNumber - 1u]);
  }
}

static void Grep_AppendNotices(StrBuf* output, u32 limit, bool limitHit, bool bytesTruncated,
                               bool linesTruncated) {
  if (!limitHit && !bytesTruncated && !linesTruncated) return;
  StrBuf_AppendCStr(output, "\n\n[");
  bool wrote = false;
  if (limitHit) {
    StrBuf_AppendU64(output, limit);
    StrBuf_AppendCStr(output, " matches limit reached. Use limit=");
    StrBuf_AppendU64(output, (u64)limit * 2u);
    StrBuf_AppendCStr(output, " for more, or refine pattern");
    wrote = true;
  }
  if (bytesTruncated) {
    if (wrote) StrBuf_AppendCStr(output, ". ");
    StrBuf_AppendCStr(output, "50.0KB limit reached");
    wrote = true;
  }
  if (linesTruncated) {
    if (wrote) StrBuf_AppendCStr(output, ". ");
    StrBuf_AppendCStr(output, "Some lines truncated to 500 chars. Use read tool to see full lines");
  }
  StrBuf_AppendChar(output, ']');
}

static bool GrepExec(void*, Arena* arena, StrView, const ToolArgs* args,
                     volatile i32* abort, const ToolUpdateSink* updates,
                     ToolResult* out, StrBuf* err) {
  (void)updates;
  const JsonValue* argsJsonValue = args->Root;
  const JsonValue* patternValue = ToolArgs_Get(args, "pattern");
  if (!Json_IsString(patternValue)) {
    StrBuf_AppendCStr(err, "missing 'pattern' argument");
    return false;
  }
  StrView pattern = patternValue->Str;
  StrView searchInput = Json_GetStr(argsJsonValue, "path");
  if (searchInput.Length == 0) searchInput = SVL(".");
  if (!Fs_Exists(searchInput)) {
    StrBuf_AppendCStr(err, "Path not found: ");
    StrBuf_Append(err, searchInput);
    return false;
  }

  StrView rgPath = ToolManager_EnsureRipgrep(arena, abort, err);
  if (rgPath.Length == 0) return false;

  StrView searchPath = Fs_RealPath(arena, searchInput);
  bool isDirectory = Fs_IsDir(searchPath);
  i64 contextArg = Json_GetInt(argsJsonValue, "context", 0);
  u32 context = contextArg > 0
                    ? (contextArg > (i64)0xFFFFFFFFu ? 0xFFFFFFFFu : (u32)contextArg)
                    : 0u;
  i64 limitArg = Json_GetInt(argsJsonValue, "limit", GREP_DEFAULT_LIMIT);
  u32 limit = limitArg < 1
                  ? 1u
                  : (limitArg > (i64)0xFFFFFFFFu ? 0xFFFFFFFFu : (u32)limitArg);
  StrView glob = Json_GetStr(argsJsonValue, "glob");

  StrView rgArgs[12];
  u32 rgArgCount = 0;
  rgArgs[rgArgCount++] = SVL("--json");
  rgArgs[rgArgCount++] = SVL("--line-number");
  rgArgs[rgArgCount++] = SVL("--color=never");
  rgArgs[rgArgCount++] = SVL("--hidden");
  if (Json_GetBool(argsJsonValue, "ignoreCase", false))
    rgArgs[rgArgCount++] = SVL("--ignore-case");
  if (Json_GetBool(argsJsonValue, "literal", false))
    rgArgs[rgArgCount++] = SVL("--fixed-strings");
  if (glob.Length) {
    rgArgs[rgArgCount++] = SVL("--glob");
    rgArgs[rgArgCount++] = glob;
  }
  rgArgs[rgArgCount++] = SVL("--");
  rgArgs[rgArgCount++] = pattern;
  rgArgs[rgArgCount++] = searchPath;

  GrepCapture capture = {};
  capture.Mem = arena;
  capture.Matches = Array_Make<GrepMatch>(arena, limit < 32u ? limit : 32u);
  capture.Pending = StrBuf_Make(arena, 4096);
  capture.Stderr = StrBuf_Make(arena, 512);
  capture.Limit = limit;
  ExecArgvSink sink = {};
  sink.User = &capture;
  sink.OnStdout = Grep_OnStdout;
  sink.OnStderr = Grep_OnStderr;
  ExecArgvResult result = Exec_RunArgv(arena, rgPath, rgArgs, rgArgCount, 0, abort, sink);
  if (!capture.LimitHit && capture.Pending.Length)
    Grep_ProcessJsonLine(&capture, StrBuf_View(&capture.Pending));

  if (abort && Atomic_LoadI32(abort)) {
    StrBuf_AppendCStr(err, "Operation aborted");
    return false;
  }
  if (!result.Spawned) {
    StrBuf_AppendCStr(err, "Failed to run ripgrep");
    return false;
  }
  if (!capture.LimitHit && result.ExitCode != 0 && result.ExitCode != 1) {
    StrView stderrText = Grep_Trim(StrBuf_View(&capture.Stderr));
    if (stderrText.Length)
      StrBuf_Append(err, stderrText);
    else {
      StrBuf_AppendCStr(err, "ripgrep exited with code ");
      StrBuf_AppendI64(err, result.ExitCode);
    }
    return false;
  }
  if (capture.MatchCount == 0) {
    out->Content = Array_Make<Content>(arena, 1);
    Array_Push(&out->Content, Content_Text(SVL("No matches found")));
    return true;
  }

  StrBuf rawOutput = StrBuf_Make(arena, 4096);
  GrepFormatter formatter = {};
  formatter.Mem = arena;
  formatter.Output = &rawOutput;
  formatter.Cache = Array_Make<GrepFileCacheEntry>(arena, 8);
  formatter.SearchPath = searchPath;
  formatter.SearchIsDirectory = isDirectory;
  formatter.Context = context;
  for (u32 i = 0; i < capture.Matches.Count; i++)
    Grep_FormatMatch(&formatter, capture.Matches.Data[i]);

  TruncResult truncation =
      Trunc_Head(arena, StrBuf_View(&rawOutput), 0xFFFFFFFFu, TRUNC_MAX_BYTES);
  StrBuf output = StrBuf_Make(arena, truncation.Content.Length + 512);
  StrBuf_Append(&output, truncation.Content);
  Grep_AppendNotices(&output, limit, capture.LimitHit,
                     truncation.Truncated && truncation.ByBytes, formatter.LinesTruncated);

  out->Content = Array_Make<Content>(arena, 1);
  Array_Push(&out->Content, Content_Text(StrBuf_View(&output)));
  return true;
}

Tool GrepTool() {
  Tool tool = {};
  tool.Name = SVL("grep");
  tool.Label = SVL("Grep");
  tool.Description =
      SVL("Search file contents for a pattern. Returns matching lines with file paths and line "
          "numbers. Respects .gitignore. Output is truncated to 100 matches or 50KB (whichever is "
          "hit first). Long lines are truncated to 500 chars.");
  tool.PromptSnippet = SVL("Search file contents for patterns (respects .gitignore)");
  tool.ParamsJsonSchema = SVL(
      "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\",\"description\":\"Search "
      "pattern (regex or literal string)\"},\"path\":{\"type\":\"string\",\"description\":\"Directory "
      "or file to search (default: current directory)\"},\"glob\":{\"type\":\"string\",\"description\":"
      "\"Filter files by glob pattern, e.g. '*.ts' or '**/*.spec.ts'\"},\"ignoreCase\":{\"type\":"
      "\"boolean\",\"default\":false,\"description\":\"Case-insensitive search\"},\"literal\":{\"type\":"
      "\"boolean\",\"default\":false,\"description\":\"Treat pattern as literal string instead of "
      "regex\"},\"context\":{\"type\":\"number\",\"default\":0,\"description\":\"Number of lines to "
      "show before and after each match\"},\"limit\":{\"type\":\"number\",\"default\":100,\"description\":"
      "\"Maximum number of matches to return\"}},\"required\":[\"pattern\"]}");
  tool.Execute = GrepExec;
  tool.PermissionEffect = ToolPermissionEffect::Inspection;
  return tool;
}
