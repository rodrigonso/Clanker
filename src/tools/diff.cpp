#include "tools/diff.h"

#include <string.h>

#include "base/array.h"

enum DiffOpKind {
  DiffOp_Equal,
  DiffOp_Delete,
  DiffOp_Insert,
};

struct DiffOp {
  u8 Kind;
  u32 OldIndex;
  u32 NewIndex;
};

struct DiffHunk {
  u32 StartOp;
  u32 EndOp;
};

static u32 Diff_CountLines(StrView text) {
  if (text.Length == 0) return 0;
  u32 count = 1;
  for (u32 i = 0; i < text.Length; i++) {
    if (text.Data[i] == '\n') count++;
  }
  if (text.Data[text.Length - 1] == '\n') count--;
  return count;
}

static StrView* Diff_SplitLines(Arena* arena, StrView text, u32* outCount) {
  u32 count = Diff_CountLines(text);
  *outCount = count;
  if (count == 0) return nullptr;

  StrView* lines = ArenaPushArray(arena, StrView, count);
  u32 lineIndex = 0;
  u32 start = 0;
  for (u32 i = 0; i < text.Length; i++) {
    if (text.Data[i] == '\n') {
      lines[lineIndex++] = StrView{text.Data + start, i - start};
      start = i + 1;
    }
  }
  if (start < text.Length) {
    lines[lineIndex++] = StrView{text.Data + start, text.Length - start};
  }
  FFI(lineIndex != count);
  return lines;
}

static u32 Diff_DigitWidth(u32 value) {
  u32 width = 1;
  while (value >= 10) {
    value /= 10;
    width++;
  }
  return width;
}

static void Diff_AppendLineNo(StrBuf* b, u32 lineNo, u32 width) {
  u32 digits = Diff_DigitWidth(lineNo);
  StrBuf_AppendU64(b, lineNo);
  while (digits < width) {
    StrBuf_AppendChar(b, ' ');
    digits++;
  }
}

static void Diff_AppendDisplayLine(StrBuf* b, char prefix, u32 lineNo, u32 width, StrView text) {
  StrBuf_AppendChar(b, prefix);
  Diff_AppendLineNo(b, lineNo, width);
  StrBuf_AppendChar(b, ' ');
  StrBuf_Append(b, text);
  StrBuf_AppendChar(b, '\n');
}

static void Diff_AppendDisplayElision(StrBuf* b, u32 width) {
  StrBuf_AppendChar(b, ' ');
  for (u32 i = 0; i < width; i++) StrBuf_AppendChar(b, ' ');
  StrBuf_AppendCStr(b, " ...\n");
}

static void Diff_AppendRange(StrBuf* b, u32 start, u32 count) {
  StrBuf_AppendU64(b, start);
  StrBuf_AppendChar(b, ',');
  StrBuf_AppendU64(b, count);
}

static void Diff_AppendPatchLine(StrBuf* b, char prefix, StrView text) {
  StrBuf_AppendChar(b, prefix);
  StrBuf_Append(b, text);
  StrBuf_AppendChar(b, '\n');
}

static Array<DiffOp> Diff_BuildOps(Arena* arena, StrView* oldLines, u32 oldCount, StrView* newLines,
                                   u32 newCount, i32* outFirstChangedLine) {
  usize rows = (usize)oldCount + 1;
  usize cols = (usize)newCount + 1;
  u32* dp = ArenaPushArray(arena, u32, rows * cols);
  memset(dp, 0, sizeof(u32) * rows * cols);

  for (u32 oi = oldCount; oi > 0; oi--) {
    u32 i = oi - 1;
    for (u32 nj = newCount; nj > 0; nj--) {
      u32 j = nj - 1;
      u32 value;
      if (StrView_Eq(oldLines[i], newLines[j])) {
        value = 1 + dp[(i + 1) * cols + (j + 1)];
      } else {
        u32 skipOld = dp[(i + 1) * cols + j];
        u32 skipNew = dp[i * cols + (j + 1)];
        value = skipOld >= skipNew ? skipOld : skipNew;
      }
      dp[i * cols + j] = value;
    }
  }

  Array<DiffOp> ops = Array_Make<DiffOp>(arena, oldCount + newCount + 1);
  *outFirstChangedLine = -1;

  u32 i = 0;
  u32 j = 0;
  while (i < oldCount || j < newCount) {
    DiffOp op = {};
    op.OldIndex = i;
    op.NewIndex = j;
    if (i < oldCount && j < newCount && StrView_Eq(oldLines[i], newLines[j])) {
      op.Kind = DiffOp_Equal;
      i++;
      j++;
    } else if (j >= newCount) {
      op.Kind = DiffOp_Delete;
      i++;
    } else if (i >= oldCount) {
      op.Kind = DiffOp_Insert;
      j++;
    } else {
      u32 skipOld = dp[(i + 1) * cols + j];
      u32 skipNew = dp[i * cols + (j + 1)];
      if (skipOld >= skipNew) {
        op.Kind = DiffOp_Delete;
        i++;
      } else {
        op.Kind = DiffOp_Insert;
        j++;
      }
    }

    if (op.Kind != DiffOp_Equal && *outFirstChangedLine < 0) {
      *outFirstChangedLine = (i32)op.NewIndex + 1;
    }
    Array_Push(&ops, op);
  }

  return ops;
}

static Array<DiffHunk> Diff_BuildHunks(Arena* arena, Array<DiffOp>* ops, u32 context) {
  Array<DiffHunk> hunks = Array_Make<DiffHunk>(arena, 8);
  u32 i = 0;
  while (i < ops->Count) {
    while (i < ops->Count && ops->Data[i].Kind == DiffOp_Equal) i++;
    if (i >= ops->Count) break;

    u32 changeStart = i;
    while (i < ops->Count && ops->Data[i].Kind != DiffOp_Equal) i++;
    u32 changeEnd = i;

    u32 start = changeStart > context ? changeStart - context : 0;
    u32 end = changeEnd + context;
    if (end > ops->Count) end = ops->Count;

    DiffHunk* last = Array_Last(&hunks);
    if (last && start <= last->EndOp) {
      if (end > last->EndOp) last->EndOp = end;
    } else {
      DiffHunk h = {};
      h.StartOp = start;
      h.EndOp = end;
      Array_Push(&hunks, h);
    }
  }
  return hunks;
}

static void Diff_HunkStats(Array<DiffOp>* ops, DiffHunk h, u32* oldStart, u32* oldCount, u32* newStart,
                           u32* newCount) {
  i32 firstOld = -1;
  i32 firstNew = -1;
  u32 oc = 0;
  u32 nc = 0;

  for (u32 i = h.StartOp; i < h.EndOp; i++) {
    DiffOp* op = &ops->Data[i];
    if (op->Kind == DiffOp_Equal || op->Kind == DiffOp_Delete) {
      if (firstOld < 0) firstOld = (i32)op->OldIndex;
      oc++;
    }
    if (op->Kind == DiffOp_Equal || op->Kind == DiffOp_Insert) {
      if (firstNew < 0) firstNew = (i32)op->NewIndex;
      nc++;
    }
  }

  DiffOp* first = &ops->Data[h.StartOp];
  *oldStart = firstOld >= 0 ? (u32)firstOld + 1 : first->OldIndex;
  *newStart = firstNew >= 0 ? (u32)firstNew + 1 : first->NewIndex;
  *oldCount = oc;
  *newCount = nc;
}

static StrView Diff_BuildDisplay(Arena* arena, Array<DiffOp>* ops, Array<DiffHunk>* hunks, StrView* oldLines,
                                 StrView* newLines, u32 oldCount, u32 newCount) {
  StrBuf b = StrBuf_Make(arena, 1024);
  u32 maxLine = oldCount > newCount ? oldCount : newCount;
  if (maxLine == 0) maxLine = 1;
  u32 width = Diff_DigitWidth(maxLine);

  u32 previousEnd = 0;
  for (u32 hi = 0; hi < hunks->Count; hi++) {
    DiffHunk h = hunks->Data[hi];
    if (h.StartOp > previousEnd) Diff_AppendDisplayElision(&b, width);

    for (u32 oi = h.StartOp; oi < h.EndOp; oi++) {
      DiffOp* op = &ops->Data[oi];
      if (op->Kind == DiffOp_Equal) {
        Diff_AppendDisplayLine(&b, ' ', op->NewIndex + 1, width, newLines[op->NewIndex]);
      } else if (op->Kind == DiffOp_Delete) {
        Diff_AppendDisplayLine(&b, '-', op->OldIndex + 1, width, oldLines[op->OldIndex]);
      } else {
        Diff_AppendDisplayLine(&b, '+', op->NewIndex + 1, width, newLines[op->NewIndex]);
      }
    }
    previousEnd = h.EndOp;
  }

  if (previousEnd < ops->Count) Diff_AppendDisplayElision(&b, width);
  return StrBuf_View(&b);
}

static StrView Diff_BuildPatch(Arena* arena, StrView path, Array<DiffOp>* ops, Array<DiffHunk>* hunks,
                               StrView* oldLines, StrView* newLines) {
  StrBuf b = StrBuf_Make(arena, 1024);
  StrBuf_AppendCStr(&b, "--- ");
  StrBuf_Append(&b, path);
  StrBuf_AppendChar(&b, '\n');
  StrBuf_AppendCStr(&b, "+++ ");
  StrBuf_Append(&b, path);
  StrBuf_AppendChar(&b, '\n');

  for (u32 hi = 0; hi < hunks->Count; hi++) {
    DiffHunk h = hunks->Data[hi];
    u32 oldStart;
    u32 oldCount;
    u32 newStart;
    u32 newCount;
    Diff_HunkStats(ops, h, &oldStart, &oldCount, &newStart, &newCount);

    StrBuf_AppendCStr(&b, "@@ -");
    Diff_AppendRange(&b, oldStart, oldCount);
    StrBuf_AppendCStr(&b, " +");
    Diff_AppendRange(&b, newStart, newCount);
    StrBuf_AppendCStr(&b, " @@\n");

    for (u32 oi = h.StartOp; oi < h.EndOp; oi++) {
      DiffOp* op = &ops->Data[oi];
      if (op->Kind == DiffOp_Equal) {
        Diff_AppendPatchLine(&b, ' ', oldLines[op->OldIndex]);
      } else if (op->Kind == DiffOp_Delete) {
        Diff_AppendPatchLine(&b, '-', oldLines[op->OldIndex]);
      } else {
        Diff_AppendPatchLine(&b, '+', newLines[op->NewIndex]);
      }
    }
  }

  return StrBuf_View(&b);
}

DiffResult Diff_Make(Arena* arena, StrView path, StrView oldText, StrView newText, u32 context) {
  DiffResult result = {};
  result.FirstChangedLine = -1;
  if (context == 0) context = 4;
  if (StrView_Eq(oldText, newText)) return result;

  u32 oldCount = 0;
  u32 newCount = 0;
  StrView* oldLines = Diff_SplitLines(arena, oldText, &oldCount);
  StrView* newLines = Diff_SplitLines(arena, newText, &newCount);

  i32 firstChangedLine = -1;
  Array<DiffOp> ops = Diff_BuildOps(arena, oldLines, oldCount, newLines, newCount, &firstChangedLine);
  Array<DiffHunk> hunks = Diff_BuildHunks(arena, &ops, context);

  result.Display = Diff_BuildDisplay(arena, &ops, &hunks, oldLines, newLines, oldCount, newCount);
  result.Patch = Diff_BuildPatch(arena, path, &ops, &hunks, oldLines, newLines);
  result.FirstChangedLine = firstChangedLine;
  return result;
}
