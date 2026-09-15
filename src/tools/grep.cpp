#include <stdio.h>
#include <string.h>

#include "base/thread.h"
#include "base/file_io.h"
#include "tools/fs.h"
#include "tools/glob.h"
#include "tools/search_regex.h"
#include "tools/tools.h"
#include "tools/truncate.h"

static const u32 GREP_DEFAULT_LIMIT = 100;
static const u32 GREP_MAX_LINE_LENGTH = 500;
static const u32 GREP_MAX_LINE_BYTES = 8 * 1024 * 1024;

struct GrepState {
  SearchRegex Regex;
  StrBuf Output;
  StrBuf* Error;
  StrView SearchPath;
  StrView Glob;
  volatile i32* Abort;
  u32 Context;
  u32 Limit;
  u32 MatchCount;
  bool Directory;
  bool LimitHit;
  bool BytesTruncated;
  bool LinesTruncated;
  bool Failed;
};

struct GrepReader {
  FILE* File;
  StrView Path;
  StrBuf Line;
  char Buffer[16384];
  u32 Position;
  u32 Count;
  u32 LineNumber;
  bool Started;
  bool Eof;
  bool Binary;
  bool Utf16;
  bool BigEndian;
};

struct GrepLine {
  StrView Text;
  u32 Number;
  bool Truncated;
};

static bool Grep_Error(GrepState* state, const char* message, StrView path) {
  StrBuf_AppendCStr(state->Error, message);
  StrBuf_Append(state->Error, path);
  return false;
}

static bool Grep_Cancelled(GrepState* state) {
  if (!state->Abort || !Atomic_LoadI32(state->Abort)) return false;
  StrBuf_AppendCStr(state->Error, "Operation aborted");
  return true;
}

static bool Grep_Fill(GrepState* state, GrepReader* reader) {
  if (Grep_Cancelled(state)) return false;
  if (reader->Position < reader->Count || reader->Eof) return true;
  reader->Position = 0;
  reader->Count = (u32)fread(reader->Buffer, 1, sizeof(reader->Buffer), reader->File);
  if (ferror(reader->File)) return Grep_Error(state, "Failed to read file: ", reader->Path);
  if (!reader->Count) reader->Eof = true;
  if (!reader->Started) {
    reader->Started = true;
    const u8* bytes = (const u8*)reader->Buffer;
    if (reader->Count >= 2 &&
        ((bytes[0] == 0xff && bytes[1] == 0xfe) ||
         (bytes[0] == 0xfe && bytes[1] == 0xff))) {
      reader->Utf16 = true;
      reader->BigEndian = bytes[0] == 0xfe;
      reader->Position = 2;
    } else if (reader->Count >= 3 && bytes[0] == 0xef &&
               bytes[1] == 0xbb && bytes[2] == 0xbf) {
      reader->Position = 3;
    }
  }
  if (!reader->Utf16 &&
      memchr(reader->Buffer + reader->Position, 0, reader->Count - reader->Position))
    reader->Binary = true;
  return true;
}

static bool Grep_Utf16Unit(GrepState* state, GrepReader* reader, u32* unit,
                            bool* available) {
  *available = false;
  if (!Grep_Fill(state, reader)) return false;
  if (reader->Eof) return true;
  u32 first = (u8)reader->Buffer[reader->Position++];
  if (!Grep_Fill(state, reader)) return false;
  if (reader->Eof) return Grep_Error(state, "Incomplete UTF-16 code unit in: ", reader->Path);
  u32 second = (u8)reader->Buffer[reader->Position++];
  *unit = reader->BigEndian ? first * 256 + second : second * 256 + first;
  *available = true;
  return true;
}

static bool Grep_ReadLine(GrepState* state, GrepReader* reader, bool* available) {
  *available = false;
  StrBuf_Clear(&reader->Line);
  bool ended = false;
  while (!ended) {
    if (!Grep_Fill(state, reader)) return false;
    if (reader->Binary) return true;
    if (reader->Eof) break;
    if (reader->Utf16) {
      u32 cp = 0;
      bool got = false;
      if (!Grep_Utf16Unit(state, reader, &cp, &got)) return false;
      if (!got) break;
      if (!cp) { reader->Binary = true; return true; }
      if (cp == '\n') { ended = true; break; }
      if (cp >= 0xd800 && cp <= 0xdbff) {
        u32 low = 0;
        if (!Grep_Utf16Unit(state, reader, &low, &got)) return false;
        if (!got || low < 0xdc00 || low > 0xdfff)
          return Grep_Error(state, "Invalid UTF-16 surrogate in: ", reader->Path);
        cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
      } else if (cp >= 0xdc00 && cp <= 0xdfff) {
        return Grep_Error(state, "Invalid UTF-16 surrogate in: ", reader->Path);
      }
      char utf8[4];
      u32 length = 0;
      if (cp < 0x80) {
        utf8[length++] = (char)cp;
      } else if (cp < 0x800) {
        utf8[length++] = (char)(0xc0 | (cp >> 6));
        utf8[length++] = (char)(0x80 | (cp & 63));
      } else if (cp < 0x10000) {
        utf8[length++] = (char)(0xe0 | (cp >> 12));
        utf8[length++] = (char)(0x80 | ((cp >> 6) & 63));
        utf8[length++] = (char)(0x80 | (cp & 63));
      } else {
        utf8[length++] = (char)(0xf0 | (cp >> 18));
        utf8[length++] = (char)(0x80 | ((cp >> 12) & 63));
        utf8[length++] = (char)(0x80 | ((cp >> 6) & 63));
        utf8[length++] = (char)(0x80 | (cp & 63));
      }
      if (length > GREP_MAX_LINE_BYTES - reader->Line.Length)
        return Grep_Error(state, "Search line exceeds 8MiB in: ", reader->Path);
      StrBuf_Append(&reader->Line, StrView{utf8, length});
    } else {
      const char* begin = reader->Buffer + reader->Position;
      u32 count = reader->Count - reader->Position;
      const char* newline = (const char*)memchr(begin, '\n', count);
      if (newline) count = (u32)(newline - begin);
      if (count > GREP_MAX_LINE_BYTES - reader->Line.Length)
        return Grep_Error(state, "Search line exceeds 8MiB in: ", reader->Path);
      StrBuf_Append(&reader->Line, StrView{begin, count});
      reader->Position += count;
      if (newline) { reader->Position++; ended = true; }
    }
  }
  if (!ended && !reader->Line.Length) return true;
  if (reader->LineNumber == 0xffffffffu)
    return Grep_Error(state, "Too many lines in: ", reader->Path);
  reader->LineNumber++;
  if (reader->Line.Length && reader->Line.Data[reader->Line.Length - 1] == '\r') {
    reader->Line.Data[--reader->Line.Length] = 0;
  }
  *available = true;
  return true;
}

static GrepLine Grep_SaveLine(Arena* arena, const GrepReader& reader) {
  char prefix[GREP_MAX_LINE_LENGTH * 4];
  u32 bytes = 0, pos = 0, chars = 0;
  while (pos < reader.Line.Length && chars < GREP_MAX_LINE_LENGTH) {
    if (reader.Line.Data[pos] == '\r') { pos++; continue; }
    u8 lead = (u8)reader.Line.Data[pos];
    u32 width = 1;
    if ((lead & 0xe0) == 0xc0) width = 2;
    else if ((lead & 0xf0) == 0xe0) width = 3;
    else if ((lead & 0xf8) == 0xf0) width = 4;
    if (pos + width > reader.Line.Length) width = 1;
    for (u32 i = 1; i < width; i++)
      if (((u8)reader.Line.Data[pos + i] & 0xc0) != 0x80) { width = 1; break; }
    memcpy(prefix + bytes, reader.Line.Data + pos, width);
    bytes += width;
    pos += width;
    chars++;
  }
  while (pos < reader.Line.Length && reader.Line.Data[pos] == '\r') pos++;
  return GrepLine{StrView_Dup(arena, StrView{prefix, bytes}),
                  reader.LineNumber, pos < reader.Line.Length};
}

static bool Grep_AppendLine(GrepState* state, StrView display,
                             const GrepLine& line, u32 match) {
  if (state->Output.Length) StrBuf_AppendChar(&state->Output, '\n');
  StrBuf_Append(&state->Output, display);
  char delimiter = line.Number == match ? ':' : '-';
  StrBuf_AppendChar(&state->Output, delimiter);
  StrBuf_AppendU64(&state->Output, line.Number);
  StrBuf_AppendChar(&state->Output, delimiter);
  StrBuf_AppendChar(&state->Output, ' ');
  StrBuf_Append(&state->Output, line.Text);
  if (line.Truncated) {
    StrBuf_AppendCStr(&state->Output, "... [truncated]");
    state->LinesTruncated = true;
  }
  if (state->Output.Length > TRUNC_MAX_BYTES) {
    state->BytesTruncated = true;
    return false;
  }
  return true;
}

static bool Grep_SearchOpenFile(GrepState* state, GrepReader* reader,
                                 Arena* scratch, StrView display) {
  if (!state->Context) {
    u32 outputStart = state->Output.Length;
    u32 matchesStart = state->MatchCount;
    bool truncatedStart = state->LinesTruncated;
    for (;;) {
      bool available = false;
      if (!Grep_ReadLine(state, reader, &available)) return false;
      if (!available) break;
      bool matched = false;
      if (!SearchRegex_Match(&state->Regex, StrBuf_View(&reader->Line),
                              &matched, state->Error)) return false;
      if (!matched) continue;
      state->MatchCount++;
      if (state->MatchCount == state->Limit) state->LimitHit = true;
      usize mark = scratch->Used;
      GrepLine line = Grep_SaveLine(scratch, *reader);
      bool more = Grep_AppendLine(state, display, line, reader->LineNumber);
      ArenaRestore(scratch, mark);
      if (!more || state->LimitHit) return true;
    }
    if (reader->Binary) {
      state->Output.Length = outputStart;
      state->Output.Data[outputStart] = 0;
      state->MatchCount = matchesStart;
      state->LinesTruncated = truncatedStart;
    }
    return true;
  }
  Array<u32> matches = Array_Make<u32>(scratch, 32);
  u32 remaining = state->Limit - state->MatchCount;
  // Every formatted match consumes at least five bytes, even for empty lines.
  u32 cap = ClankerMin(remaining, TRUNC_MAX_BYTES / 5 + 1);
  bool available = false;
  while (matches.Count < cap) {
    if (!Grep_ReadLine(state, reader, &available)) return false;
    if (!available) break;
    bool matched = false;
    if (!SearchRegex_Match(&state->Regex, StrBuf_View(&reader->Line),
                            &matched, state->Error)) return false;
    if (matched) Array_Push(&matches, reader->LineNumber);
  }
  if (reader->Binary || !matches.Count) return true;
  if (matches.Count == remaining) state->LimitHit = true;
  state->MatchCount += matches.Count;
  if (fseek(reader->File, 0, SEEK_SET) != 0)
    return Grep_Error(state, "Failed to rewind file: ", reader->Path);
  reader->Position = reader->Count = reader->LineNumber = 0;
  reader->Started = reader->Eof = reader->Utf16 = reader->BigEndian = false;

  // Cache only displayed, clipped lines. Overlapping context is replayed in
  // match order without retaining whole files or rereading each match window.
  Array<GrepLine> lines = Array_Make<GrepLine>(scratch, 32);
  u32 cacheStart = 0;
  for (u32 i = 0; i < matches.Count; i++) {
    if (Grep_Cancelled(state)) return false;
    u32 match = matches.Data[i];
    u32 first = match > state->Context ? match - state->Context : 1;
    u64 last = (u64)match + state->Context;
    while (cacheStart < lines.Count && lines.Data[cacheStart].Number < first) cacheStart++;
    for (u32 j = cacheStart; j < lines.Count && lines.Data[j].Number <= last; j++)
      if (!Grep_AppendLine(state, display, lines.Data[j], match)) return true;
    while ((u64)reader->LineNumber < last) {
      if (!Grep_ReadLine(state, reader, &available)) return false;
      if (reader->Binary) return Grep_Error(state, "File became binary during search: ", reader->Path);
      if (!available) break;
      if (reader->LineNumber < first) continue;
      GrepLine line = Grep_SaveLine(scratch, *reader);
      Array_Push(&lines, line);
      if (!Grep_AppendLine(state, display, line, match)) return true;
    }
    if (reader->LineNumber < match)
      return Grep_Error(state, "File changed during search: ", reader->Path);
  }
  return true;
}

static bool Grep_PathMatches(GrepState* state, StrView relative, bool* matched) {
  *matched = true;
  StrView glob = state->Glob;
  if (!glob.Length) return true;
  bool exclude = glob.Data[0] == '!';
  if (exclude) { glob.Data++; glob.Length--; }
  bool anchored = glob.Length && glob.Data[0] == '/';
  if (anchored) { glob.Data++; glob.Length--; }
  bool pathPattern = anchored || memchr(glob.Data, '/', glob.Length) != nullptr;
  bool match = false;
  if (!Glob_MatchChecked(glob, pathPattern ? relative : Fs_Base(relative), false, &match))
    return Grep_Error(state, "Search glob complexity limit exceeded: ", state->Glob);
  *matched = exclude ? !match : match;
  return true;
}

static bool Grep_SearchFile(GrepState* state, StrView path, StrView display) {
  bool matched = false;
  if (!Grep_PathMatches(state, display, &matched)) return false;
  if (!matched) return true;
  if (Grep_Cancelled(state)) return false;
  Arena scratch = ArenaCreate();
  Arena lineArena = ArenaCreate((usize)32 << 20);
  FILE* file = nullptr;
  FileIoResult opened = File_Open(&scratch, path, "rb", &file);
  bool ok = false;
  if (opened != FileIoResult::Success) {
    Grep_Error(state, "Failed to open file: ", path);
  } else {
    GrepReader reader = {};
    reader.File = file;
    reader.Path = path;
    reader.Line = StrBuf_Make(&lineArena, 4096);
    ok = Grep_SearchOpenFile(state, &reader, &scratch, display);
    if (fclose(file) != 0 && ok)
      ok = Grep_Error(state, "Failed to close file: ", path);
  }
  ArenaRelease(&lineArena);
  ArenaRelease(&scratch);
  return ok;
}

static bool Grep_FilterDirectory(void* user, StrView relative, const FsEntry& entry,
                                  bool* include, StrBuf*) {
  *include = true;
  return !entry.IsDir || Grep_PathMatches((GrepState*)user, relative, include);
}

static bool Grep_Visit(void* user, StrView absolute, StrView relative,
                       const FsEntry& entry) {
  GrepState* state = (GrepState*)user;
  if (!entry.IsRegular || entry.IsSymlink) return true;
  if (!Grep_SearchFile(state, absolute, relative)) state->Failed = true;
  return !state->Failed && !state->LimitHit && !state->BytesTruncated;
}

static void Grep_AppendNotices(StrBuf* output, const GrepState& state) {
  if (!state.LimitHit && !state.BytesTruncated && !state.LinesTruncated) return;
  StrBuf_AppendCStr(output, "\n\n[");
  bool wrote = false;
  if (state.LimitHit) {
    StrBuf_AppendU64(output, state.Limit);
    StrBuf_AppendCStr(output, " matches limit reached. Use limit=");
    StrBuf_AppendU64(output, (u64)state.Limit * 2);
    StrBuf_AppendCStr(output, " for more, or refine pattern");
    wrote = true;
  }
  if (state.BytesTruncated) {
    if (wrote) StrBuf_AppendCStr(output, ". ");
    StrBuf_AppendCStr(output, "50.0KB limit reached");
    wrote = true;
  }
  if (state.LinesTruncated) {
    if (wrote) StrBuf_AppendCStr(output, ". ");
    StrBuf_AppendCStr(output, "Some lines truncated to 500 chars. Use read tool to see full lines");
  }
  StrBuf_AppendChar(output, ']');
}

static bool GrepExec(void*, Arena* arena, StrView, const ToolArgs* args,
                     volatile i32* abort, const ToolUpdateSink*,
                     ToolResult* out, StrBuf* err) {
  StrView pattern = ToolArgs_GetStr(args, "pattern");
  StrView input = ToolArgs_GetStr(args, "path");
  if (!input.Length) input = SVL(".");
  if (memchr(input.Data, 0, input.Length)) {
    StrBuf_AppendCStr(err, "Search path contains a NUL byte");
    return false;
  }
  FsStat stat = Fs_Stat(input);
  if (!stat.Exists) {
    StrBuf_AppendCStr(err, "Path not found: ");
    StrBuf_Append(err, input);
    return false;
  }
  if (!stat.IsDir && !stat.IsRegular) {
    StrBuf_AppendCStr(err, "Search path is not a regular file or directory");
    return false;
  }

  Arena scratch = ArenaCreate();
  GrepState state = {};
  state.Error = err;
  state.Abort = abort;
  state.SearchPath = Fs_RealPath(&scratch, input);
  state.Directory = stat.IsDir;
  state.Glob = ToolArgs_GetStr(args, "glob");
  i64 limit = ToolArgs_GetInt(args, "limit", GREP_DEFAULT_LIMIT);
  state.Limit = limit < 1 ? 1 : (u32)ClankerMin((u64)limit, (u64)0xffffffffu);
  i64 context = ToolArgs_GetInt(args, "context", 0);
  state.Context = context < 1 ? 0 : (u32)ClankerMin((u64)context, (u64)0xffffffffu);
  state.Output = StrBuf_Make(&scratch, 4096);
  bool ok = !Grep_Cancelled(&state) && Glob_Validate(state.Glob, err) &&
            SearchRegex_Compile(&scratch, pattern, Json_GetBool(args->Root, "literal", false),
                                 Json_GetBool(args->Root, "ignoreCase", false), abort,
                                 &state.Regex, err);
  if (ok && state.Directory) {
    FsWalkOptions options = {};
    options.SkipLinks = true;
    options.Abort = abort;
    options.RespectGitignore = !state.Glob.Length || state.Glob.Data[0] == '!';
    options.RespectIgnoreFiles = options.RespectGitignore;
    options.LoadParentIgnores = true;
    if (state.Glob.Length && state.Glob.Data[0] == '!') {
      options.Filter = Grep_FilterDirectory;
      options.FilterUser = &state;
    }
    ok = Fs_WalkChecked(&scratch, state.SearchPath, options, Grep_Visit, &state, err);
    ok = ok && !state.Failed;
  } else if (ok) {
    ok = Grep_SearchFile(&state, state.SearchPath, Fs_Base(state.SearchPath));
  }
  if (ok) {
    TruncResult truncated = Trunc_Head(arena, StrBuf_View(&state.Output), 0xffffffffu,
                                      TRUNC_MAX_BYTES);
    StrBuf output = StrBuf_Make(arena, truncated.Content.Length + 512);
    if (!state.MatchCount) StrBuf_AppendCStr(&output, "No matches found");
    else StrBuf_Append(&output, truncated.Content);
    Grep_AppendNotices(&output, state);
    out->Content = Array_Make<Content>(arena, 1);
    Array_Push(&out->Content, Content_Text(StrBuf_View(&output)));
  }
  ArenaRelease(&scratch);
  return ok;
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
