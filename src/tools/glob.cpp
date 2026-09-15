#include "tools/glob.h"

struct GlobSegment {
  const char* Data;
  u32 Pos;
  u32 End;
};

static const u32 GLOB_MAX_SEGMENTS = 64;
static const u32 GLOB_MAX_DEPTH = 64;

struct GlobWork {
  u32 Remaining;
  bool Limited;
};

static bool Glob_Step(GlobWork* work) {
  if (work->Remaining) {
    work->Remaining--;
    return true;
  }
  work->Limited = true;
  return false;
}

static bool Glob_CharEq(char a, char b, bool caseInsensitive) {
  if (caseInsensitive) {
    a = Ascii_ToLower(a);
    b = Ascii_ToLower(b);
  }
  return a == b;
}

static void Glob_CopySegments(GlobSegment* dst, const GlobSegment* src, u32 count) {
  for (u32 i = 0; i < count; i++) dst[i] = src[i];
}

static void Glob_TrimEmptyLeadingSegments(GlobSegment* segs, u32* count) {
  while (*count > 0 && segs[0].Pos >= segs[0].End) {
    for (u32 i = 1; i < *count; i++) segs[i - 1] = segs[i];
    *count -= 1;
  }
}

static u32 Glob_FindClassEnd(const GlobSegment* seg, GlobWork* work) {
  u32 i = seg->Pos + 1;
  if (i < seg->End && (seg->Data[i] == '!' || seg->Data[i] == '^')) i++;
  if (i < seg->End && seg->Data[i] == ']') i++;
  for (; i < seg->End; i++) {
    if (!Glob_Step(work)) return seg->End;
    if (seg->Data[i] == '\\' && i + 1 < seg->End) { i++; continue; }
    if (seg->Data[i] == ']') return i;
  }
  return seg->End;
}

static u32 Glob_FindBraceEnd(const GlobSegment* seg, GlobWork* work) {
  for (u32 i = seg->Pos + 1; i < seg->End; i++) {
    if (!Glob_Step(work)) return seg->End;
    if (seg->Data[i] == '\\' && i + 1 < seg->End) { i++; continue; }
    if (seg->Data[i] == '[') {
      GlobSegment characterClass = {seg->Data, i, seg->End};
      u32 end = Glob_FindClassEnd(&characterClass, work);
      if (end < seg->End) { i = end; continue; }
    }
    if (seg->Data[i] == '}') return i;
  }
  return seg->End;
}

static bool Glob_ClassHasChar(const char* data, u32 start, u32 end, char ch,
                              bool caseInsensitive, GlobWork* work) {
  u32 i = start;
  if (i < end && data[i] == ']') {
    if (Glob_CharEq(data[i], ch, caseInsensitive)) return true;
    i++;
  }

  while (i < end) {
    if (!Glob_Step(work)) return false;
    char lo = data[i++];
    if (lo == '\\' && i < end) lo = data[i++];
    if (i + 1 < end && data[i] == '-') {
      i++;
      char hi = data[i++];
      if (hi == '\\' && i < end) hi = data[i++];
      char c = ch;
      if (caseInsensitive) {
        lo = Ascii_ToLower(lo);
        hi = Ascii_ToLower(hi);
        c = Ascii_ToLower(c);
      }
      if ((lo <= c && c <= hi) || (hi <= c && c <= lo)) return true;
    } else {
      if (Glob_CharEq(lo, ch, caseInsensitive)) return true;
    }
  }

  return false;
}

static bool Glob_MatchSegments(const GlobSegment* input, u32 inputCount, const char* path, u32 pathLen,
                               u32 pathPos, bool caseInsensitive, u32 depth, GlobWork* work);

static bool Glob_TryBraceAlternatives(const GlobSegment* segs, u32 count, const char* path, u32 pathLen,
                                      u32 pathPos, bool caseInsensitive, u32 depth, GlobWork* work) {
  const GlobSegment* seg = segs;
  u32 close = Glob_FindBraceEnd(seg, work);
  if (work->Limited) return false;

  u32 altStart = seg->Pos + 1;
  for (u32 i = altStart; i <= close; i++) {
    if (!Glob_Step(work)) return false;
    if (i < close && seg->Data[i] == '\\' && i + 1 < close) { i++; continue; }
    if (i < close && seg->Data[i] == '[') {
      GlobSegment characterClass = {seg->Data, i, close};
      u32 end = Glob_FindClassEnd(&characterClass, work);
      if (end < close) { i = end; continue; }
    }
    if (i == close || seg->Data[i] == ',') {
      GlobSegment next[GLOB_MAX_SEGMENTS];
      u32 nextCount = 0;

      if (altStart < i) {
        if (nextCount >= GLOB_MAX_SEGMENTS) { work->Limited = true; return false; }
        next[nextCount++] = GlobSegment{seg->Data, altStart, i};
      }
      if (close + 1 < seg->End) {
        if (nextCount >= GLOB_MAX_SEGMENTS) { work->Limited = true; return false; }
        next[nextCount++] = GlobSegment{seg->Data, close + 1, seg->End};
      }
      for (u32 rest = 1; rest < count; rest++) {
        if (nextCount >= GLOB_MAX_SEGMENTS) { work->Limited = true; return false; }
        next[nextCount++] = segs[rest];
      }

      if (Glob_MatchSegments(next, nextCount, path, pathLen, pathPos, caseInsensitive, depth + 1, work)) {
        return true;
      }
      if (work->Limited) return false;
      altStart = i + 1;
    }
  }

  return false;
}

static bool Glob_MatchSegments(const GlobSegment* input, u32 inputCount, const char* path, u32 pathLen,
                               u32 pathPos, bool caseInsensitive, u32 depth, GlobWork* work) {
  if (depth > GLOB_MAX_DEPTH || inputCount > GLOB_MAX_SEGMENTS) {
    work->Limited = true;
    return false;
  }

  GlobSegment segs[GLOB_MAX_SEGMENTS];
  Glob_CopySegments(segs, input, inputCount);
  u32 count = inputCount;

  for (;;) {
    if (!Glob_Step(work)) return false;
    Glob_TrimEmptyLeadingSegments(segs, &count);
    if (count == 0) return pathPos == pathLen;

    GlobSegment* seg = &segs[0];
    char pc = seg->Data[seg->Pos];

    if (pc == '*') {
      bool doubleStar = seg->Pos + 1 < seg->End && seg->Data[seg->Pos + 1] == '*';
      GlobSegment next[GLOB_MAX_SEGMENTS];
      Glob_CopySegments(next, segs, count);
      next[0].Pos += doubleStar ? 2u : 1u;
      while (next[0].Pos < next[0].End && next[0].Data[next[0].Pos] == '*')
        next[0].Pos++;

      // Brace alternatives are slices; use the matched prefix's component boundary.
      bool directoryStar = doubleStar && (pathPos == 0 || path[pathPos - 1] == '/') &&
                           next[0].Pos < next[0].End && next[0].Data[next[0].Pos] == '/';
      if (directoryStar) {
        next[0].Pos++;
        if (Glob_MatchSegments(next, count, path, pathLen, pathPos, caseInsensitive, depth + 1, work))
          return true;
        for (u32 i = pathPos; i < pathLen && !work->Limited; i++) {
          if (!Glob_Step(work)) return false;
          if (path[i] == '/' &&
              Glob_MatchSegments(next, count, path, pathLen, i + 1, caseInsensitive, depth + 1, work))
            return true;
        }
        return false;
      }

      if (doubleStar) {
        for (u32 i = pathPos; i <= pathLen; i++) {
          if (Glob_MatchSegments(next, count, path, pathLen, i, caseInsensitive, depth + 1, work)) return true;
          if (work->Limited) return false;
        }
      } else {
        for (u32 i = pathPos; i <= pathLen; i++) {
          if (Glob_MatchSegments(next, count, path, pathLen, i, caseInsensitive, depth + 1, work)) return true;
          if (work->Limited) return false;
          if (i == pathLen || path[i] == '/') break;
        }
      }
      return false;
    }

    if (pc == '?') {
      if (pathPos >= pathLen || path[pathPos] == '/') return false;
      seg->Pos++;
      pathPos++;
      continue;
    }

    if (pc == '[') {
      if (pathPos >= pathLen || path[pathPos] == '/') return false;
      u32 close = Glob_FindClassEnd(seg, work);
      if (work->Limited) return false;
      if (close == seg->End) {
        if (!Glob_CharEq(pc, path[pathPos], caseInsensitive)) return false;
        seg->Pos++;
        pathPos++;
        continue;
      }

      u32 classStart = seg->Pos + 1;
      bool negated = false;
      if (classStart < close && (seg->Data[classStart] == '!' || seg->Data[classStart] == '^')) {
        negated = true;
        classStart++;
      }

      bool hasChar = Glob_ClassHasChar(seg->Data, classStart, close, path[pathPos], caseInsensitive, work);
      if (work->Limited) return false;
      if (negated ? hasChar : !hasChar) return false;
      seg->Pos = close + 1;
      pathPos++;
      continue;
    }

    if (pc == '{') {
      u32 close = Glob_FindBraceEnd(seg, work);
      if (work->Limited) return false;
      if (close != seg->End) {
        return Glob_TryBraceAlternatives(segs, count, path, pathLen, pathPos, caseInsensitive, depth + 1, work);
      }
      if (pathPos >= pathLen || !Glob_CharEq(pc, path[pathPos], caseInsensitive)) return false;
      seg->Pos++;
      pathPos++;
      continue;
    }

    if (pc == '\\' && seg->Pos + 1 < seg->End) pc = seg->Data[++seg->Pos];
    if (pathPos >= pathLen || !Glob_CharEq(pc, path[pathPos], caseInsensitive)) return false;
    seg->Pos++;
    pathPos++;
  }
}

bool Glob_Match(StrView pattern, StrView path, bool caseInsensitive) {
  bool matched = false;
  return Glob_MatchChecked(pattern, path, caseInsensitive, &matched) && matched;
}

bool Glob_MatchChecked(StrView pattern, StrView path, bool caseInsensitive, bool* matched) {
  *matched = false;
  if (pattern.Length > 16384 || path.Length > 32768) return false;
  GlobWork work = {262144, false};
  GlobSegment seg = GlobSegment{pattern.Data, 0, pattern.Length};
  *matched = Glob_MatchSegments(&seg, 1, path.Data, path.Length, 0, caseInsensitive, 0, &work);
  return !work.Limited;
}

static bool Glob_ParseError(StrBuf* err, const char* message) {
  StrBuf_AppendCStr(err, "glob parse error: ");
  StrBuf_AppendCStr(err, message);
  return false;
}

bool Glob_Validate(StrView pattern, StrBuf* err) {
  if (pattern.Length > 16384) return Glob_ParseError(err, "pattern exceeds 16 KiB limit");
  bool brace = false;
  for (u32 i = 0; i < pattern.Length; i++) {
    char ch = pattern.Data[i];
    if (!ch) return Glob_ParseError(err, "NUL byte in pattern");
    if (ch == '\\') {
      if (++i == pattern.Length) return Glob_ParseError(err, "dangling escape");
      if (!pattern.Data[i]) return Glob_ParseError(err, "NUL byte in pattern");
    } else if (ch == '{') {
      if (brace) return Glob_ParseError(err, "nested brace alternatives are unsupported");
      brace = true;
    } else if (ch == '}') {
      if (!brace) return Glob_ParseError(err, "unmatched closing brace");
      brace = false;
    } else if (ch == '[') {
      u32 start = ++i;
      if (i < pattern.Length && (pattern.Data[i] == '!' || pattern.Data[i] == '^')) i++;
      if (i < pattern.Length && pattern.Data[i] == ']') i++;
      for (; i < pattern.Length && pattern.Data[i] != ']'; i++) {
        if (!pattern.Data[i]) return Glob_ParseError(err, "NUL byte in character class");
        if (pattern.Data[i] == '\\') {
          if (++i == pattern.Length) break;
          if (!pattern.Data[i]) return Glob_ParseError(err, "NUL byte in character class");
        }
      }
      if (i == pattern.Length || i == start)
        return Glob_ParseError(err, "unclosed or empty character class");
    }
  }
  return brace ? Glob_ParseError(err, "unclosed brace alternatives") : true;
}
