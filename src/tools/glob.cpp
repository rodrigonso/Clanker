#include "tools/glob.h"

struct GlobSegment {
  const char* Data;
  u32 Pos;
  u32 End;
};

static const u32 GLOB_MAX_SEGMENTS = 64;
static const u32 GLOB_MAX_DEPTH = 2048;

static char Glob_LowerAscii(char c) {
  if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
  return c;
}

static bool Glob_CharEq(char a, char b, bool caseInsensitive) {
  if (caseInsensitive) {
    a = Glob_LowerAscii(a);
    b = Glob_LowerAscii(b);
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

static u32 Glob_FindClassEnd(const GlobSegment* seg) {
  u32 i = seg->Pos + 1;
  if (i < seg->End && (seg->Data[i] == '!' || seg->Data[i] == '^')) i++;
  if (i < seg->End && seg->Data[i] == ']') i++;
  for (; i < seg->End; i++) {
    if (seg->Data[i] == ']') return i;
  }
  return seg->End;
}

static u32 Glob_FindBraceEnd(const GlobSegment* seg) {
  for (u32 i = seg->Pos + 1; i < seg->End; i++) {
    if (seg->Data[i] == '}') return i;
  }
  return seg->End;
}

static bool Glob_ClassHasChar(const char* data, u32 start, u32 end, char ch, bool caseInsensitive) {
  u32 i = start;
  if (i < end && data[i] == ']') {
    if (Glob_CharEq(data[i], ch, caseInsensitive)) return true;
    i++;
  }

  while (i < end) {
    if (i + 2 < end && data[i + 1] == '-') {
      char lo = data[i];
      char hi = data[i + 2];
      char c = ch;
      if (caseInsensitive) {
        lo = Glob_LowerAscii(lo);
        hi = Glob_LowerAscii(hi);
        c = Glob_LowerAscii(c);
      }
      if ((lo <= c && c <= hi) || (hi <= c && c <= lo)) return true;
      i += 3;
    } else {
      if (Glob_CharEq(data[i], ch, caseInsensitive)) return true;
      i++;
    }
  }

  return false;
}

static bool Glob_MatchSegments(const GlobSegment* input, u32 inputCount, const char* path, u32 pathLen,
                               u32 pathPos, bool caseInsensitive, u32 depth);

static bool Glob_TryBraceAlternatives(const GlobSegment* segs, u32 count, const char* path, u32 pathLen,
                                      u32 pathPos, bool caseInsensitive, u32 depth) {
  const GlobSegment* seg = segs;
  u32 close = Glob_FindBraceEnd(seg);

  u32 altStart = seg->Pos + 1;
  for (u32 i = altStart; i <= close; i++) {
    if (i == close || seg->Data[i] == ',') {
      GlobSegment next[GLOB_MAX_SEGMENTS];
      u32 nextCount = 0;

      if (altStart < i) {
        if (nextCount >= GLOB_MAX_SEGMENTS) return false;
        next[nextCount++] = GlobSegment{seg->Data, altStart, i};
      }
      if (close + 1 < seg->End) {
        if (nextCount >= GLOB_MAX_SEGMENTS) return false;
        next[nextCount++] = GlobSegment{seg->Data, close + 1, seg->End};
      }
      for (u32 rest = 1; rest < count; rest++) {
        if (nextCount >= GLOB_MAX_SEGMENTS) return false;
        next[nextCount++] = segs[rest];
      }

      if (Glob_MatchSegments(next, nextCount, path, pathLen, pathPos, caseInsensitive, depth + 1)) {
        return true;
      }
      altStart = i + 1;
    }
  }

  return false;
}

static bool Glob_MatchSegments(const GlobSegment* input, u32 inputCount, const char* path, u32 pathLen,
                               u32 pathPos, bool caseInsensitive, u32 depth) {
  if (depth > GLOB_MAX_DEPTH || inputCount > GLOB_MAX_SEGMENTS) return false;

  GlobSegment segs[GLOB_MAX_SEGMENTS];
  Glob_CopySegments(segs, input, inputCount);
  u32 count = inputCount;

  for (;;) {
    Glob_TrimEmptyLeadingSegments(segs, &count);
    if (count == 0) return pathPos == pathLen;

    GlobSegment* seg = &segs[0];
    char pc = seg->Data[seg->Pos];

    if (pc == '*') {
      bool doubleStar = seg->Pos + 1 < seg->End && seg->Data[seg->Pos + 1] == '*';
      GlobSegment next[GLOB_MAX_SEGMENTS];
      Glob_CopySegments(next, segs, count);
      next[0].Pos += doubleStar ? 2u : 1u;

      if (doubleStar) {
        for (u32 i = pathPos; i <= pathLen; i++) {
          if (Glob_MatchSegments(next, count, path, pathLen, i, caseInsensitive, depth + 1)) return true;
        }
      } else {
        for (u32 i = pathPos; i <= pathLen; i++) {
          if (Glob_MatchSegments(next, count, path, pathLen, i, caseInsensitive, depth + 1)) return true;
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
      u32 close = Glob_FindClassEnd(seg);
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

      bool hasChar = Glob_ClassHasChar(seg->Data, classStart, close, path[pathPos], caseInsensitive);
      if (negated ? hasChar : !hasChar) return false;
      seg->Pos = close + 1;
      pathPos++;
      continue;
    }

    if (pc == '{') {
      if (Glob_FindBraceEnd(seg) != seg->End) {
        return Glob_TryBraceAlternatives(segs, count, path, pathLen, pathPos, caseInsensitive, depth + 1);
      }
      if (pathPos >= pathLen || !Glob_CharEq(pc, path[pathPos], caseInsensitive)) return false;
      seg->Pos++;
      pathPos++;
      continue;
    }

    if (pathPos >= pathLen || !Glob_CharEq(pc, path[pathPos], caseInsensitive)) return false;
    seg->Pos++;
    pathPos++;
  }
}

bool Glob_Match(StrView pattern, StrView path, bool caseInsensitive) {
  // Edge cases: ("*.ts","a.ts") true; ("*.ts","src/a.ts") false;
  // ("**/*.ts","src/a.ts") true; ("src/**","src/a/b.ts") true;
  // ("a?c","abc") true; ("a?c","a/c") false; ("[abc]x","bx") true;
  // ("{a,b}.ts","b.ts") true; ("**","anything/here") true.
  GlobSegment seg = GlobSegment{pattern.Data, 0, pattern.Length};
  return Glob_MatchSegments(&seg, 1, path.Data, path.Length, 0, caseInsensitive, 0);
}
