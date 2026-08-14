#include "tui/linemodel.h"

#include "base/sysinfo.h"

#include <string.h>
#include <time.h>

#include "tui/markdown.h"

namespace {

const usize LINE_ROWS_RESERVE = (usize)64 * 1024 * 1024;
const u32 ICON_FIELD_CELLS = 2;
const u32 MARKER_LEFT_PADDING = 1;

// Bullet color scheme: orange for user messages and white for assistant
// responses and tool calls. Tool calls use an outlined marker; failed tool
// calls keep the outline but turn red.
const u8 ACCENT_FG = 214;
const u8 ASSISTANT_FG = 255;
// Diff stats are the one non-error place red appears: it is paired with green
// and reads as "lines removed", not as a failure. Errors still own bold 196.
const u8 DIFF_ADD_FG = 71;
const u8 DIFF_DEL_FG = 167;
StrView FilledMarker(const LineModel* m) {
  return m->Rounded ? SVL("\xe2\x97\x8f") : SVL("\xe2\x97\xbc");
}
StrView OutlineMarker(const LineModel* m) {
  return m->Rounded ? SVL("\xe2\x97\x8b") : SVL("\xe2\x97\xbb");
}

StrView BufView(const StrBuf& buf) { return StrView{buf.Data, buf.Length}; }

u32 LineTextWidth(StrView text) {
  u32 width = 0;
  u32 i = 0;
  while (i < text.Length) width += CharCellWidth(Utf8Decode(text, &i));
  return width;
}

void WrapSpanLine(Arena* a, const SpanLine* in, u16 width, Array<SpanLine>* out);

Style LStyle(const LineModel* m, u8 fg, u8 attr) {
  return Style{m->Color ? fg : COL_DEFAULT, COL_DEFAULT, attr};
}

StrView Spaces(Arena* arena, u32 count) {
  switch (count) {
    case 0: return StrView{};
    case 1: return SVL(" ");
    case 2: return SVL("  ");
    case 3: return SVL("   ");
    case 4: return SVL("    ");
  }
  char* text = (char*)ArenaAlloc(arena, count, 1);
  memset(text, ' ', count);
  return StrView{text, count};
}

u32 IconFieldWidth(StrView marker) {
  u32 width = LineTextWidth(marker);
  return width < ICON_FIELD_CELLS ? ICON_FIELD_CELLS : width;
}

StrView IconIndent(Arena* arena, StrView marker, u32 leftPadding = 0) {
  return Spaces(arena, leftPadding + IconFieldWidth(marker) + 1u);
}

void PushIcon(Arena* arena, Array<Span>* spans, StrView marker, Style style,
              u32 leftPadding = 0) {
  if (leftPadding)
    Array_Push(spans, Span{Spaces(arena, leftPadding), StyleDefault(), true});
  Array_Push(spans, Span{marker, style});
  u32 markerWidth = LineTextWidth(marker);
  u32 gap = (markerWidth < ICON_FIELD_CELLS ? ICON_FIELD_CELLS : markerWidth) - markerWidth + 1u;
  Array_Push(spans, Span{Spaces(arena, gap), style, true});
}

void InitRow(Arena* a, SpanLine* row, u32 spanCap = 4) {
  *row = {};
  row->Spans = Array_Make<Span>(a, spanCap);
}

void StartWrappedRow(Arena* a, const SpanLine* in, SpanLine* row, bool first, bool showRight,
                     u32 rightWidth, u16 width, u16* col, u16* limit) {
  InitRow(a, row);
  if (first && showRight) {
    row->Right = in->Right;
    row->HasRight = true;
  }

  *limit = width;
  if (first && showRight) *limit = (u16)(width - rightWidth - 1u);

  *col = 0;
  if (!first && in->HasContinuation) {
    u32 continuationWidth = LineTextWidth(in->Continuation.Text);
    if (continuationWidth < *limit) {
      Array_Push(&row->Spans, in->Continuation);
      *col = (u16)continuationWidth;
    }
  }
}

void AddBlank(LineModel* m) {
  SpanLine row;
  InitRow(&m->RowsMem, &row, 1);
  Array_Push(&m->Lines, row);
}

void WrapAndAppend(LineModel* m, SpanLine* line) {
  WrapSpanLine(&m->RowsMem, line, m->Width, &m->Lines);
}

void AppendPlainRows(Arena* rowsMem, Array<SpanLine>* out, u16 width, StrView text, Style style,
                     StrView prefix, StrView continuation) {
  if (text.Length == 0) return;
  u32 i = 0;
  while (i <= text.Length) {
    u32 start = i;
    while (i < text.Length && text.Data[i] != '\n') i++;
    StrView line{text.Data + start, i - start};
    bool atEnd = i >= text.Length;
    i++;

    SpanLine logical;
    InitRow(rowsMem, &logical, 2);
    if (prefix.Length) Array_Push(&logical.Spans, Span{prefix, style, true});
    if (line.Length) Array_Push(&logical.Spans, Span{line, style});
    if (continuation.Length) {
      logical.Continuation = Span{continuation, style, true};
      logical.HasContinuation = true;
    }
    WrapSpanLine(rowsMem, &logical, width, out);
    if (atEnd) break;
  }
}

void AppendPlain(LineModel* m, StrView text, Style style, StrView prefix, StrView continuation) {
  AppendPlainRows(&m->RowsMem, &m->Lines, m->Width, text, style, prefix, continuation);
}

bool EntryVisible(const Entry* entry) {
  if (entry->Kind == EntryKind::Assistant) return entry->Text.Content.Length != 0;
  return true;
}

const Entry* PreviousVisible(const LineModel* m, u32 idx) {
  while (idx > 0) {
    idx--;
    const Entry* entry = &m->Entries.Data[idx];
    if (EntryVisible(entry)) return entry;
  }
  return nullptr;
}

bool NeedsSeparator(const Entry* previous, const Entry* current) {
  if (!previous) return false;
  if (previous->Kind == EntryKind::Tool && current->Kind == EntryKind::Tool) return false;
  return true;
}

StrView FormatTimestamp(Arena* arena, u64 timestampMs) {
  struct tm local = {};
  if (!Sys_LocalTime(timestampMs, &local)) return StrView{};
  char* text = (char*)ArenaAlloc(arena, 5, 1);
  text[0] = (char)('0' + local.tm_hour / 10);
  text[1] = (char)('0' + local.tm_hour % 10);
  text[2] = ':';
  text[3] = (char)('0' + local.tm_min / 10);
  text[4] = (char)('0' + local.tm_min % 10);
  return StrView{text, 5};
}

void BuildUser(LineModel* m, const Entry* entry) {
  StrView text = BufView(entry->Text.Content);
  StrView marker = FilledMarker(m);
  StrView indent = IconIndent(&m->RowsMem, marker, MARKER_LEFT_PADDING);
  u32 i = 0;
  bool firstLine = true;
  while (i <= text.Length) {
    u32 start = i;
    while (i < text.Length && text.Data[i] != '\n') i++;
    StrView line{text.Data + start, i - start};
    bool atEnd = i >= text.Length;
    i++;

    SpanLine logical;
    InitRow(&m->RowsMem, &logical, 3);
    if (firstLine) {
      PushIcon(&m->RowsMem, &logical.Spans, marker,
               LStyle(m, ACCENT_FG, ATTR_BOLD), MARKER_LEFT_PADDING);
    } else {
      Array_Push(&logical.Spans, Span{indent, StyleDefault(), true});
    }
    if (line.Length) Array_Push(&logical.Spans, Span{line, StyleDefault()});
    logical.Continuation = Span{indent, StyleDefault(), true};
    logical.HasContinuation = true;
    if (firstLine && entry->Text.Timestamp.Length) {
      logical.Right =
          Span{entry->Text.Timestamp, LStyle(m, 244, ATTR_NONE)};
      logical.HasRight = true;
    }
    WrapAndAppend(m, &logical);
    firstLine = false;
    if (atEnd) break;
  }
}

void BuildAssistant(LineModel* m, const Entry* entry) {
  StrView text = BufView(entry->Text.Content);
  if (text.Length == 0) return;
  StrView marker = FilledMarker(m);
  StrView indent = IconIndent(&m->RowsMem, marker, MARKER_LEFT_PADDING);
  u32 indentWidth = LineTextWidth(indent);

  Array<SpanLine> logical = Array_Make<SpanLine>(&m->RowsMem, 8);
  u16 contentWidth = m->Width > indentWidth ? (u16)(m->Width - indentWidth) : (u16)1;
  Markdown_Render(&m->RowsMem, text, &logical,
                  MarkdownOpts_Make(contentWidth, m->Color));
  for (u32 i = 0; i < logical.Count; i++) {
    SpanLine prefixed;
    InitRow(&m->RowsMem, &prefixed, logical.Data[i].Spans.Count + 2);
    if (i == 0) {
      PushIcon(&m->RowsMem, &prefixed.Spans, marker,
               LStyle(m, ASSISTANT_FG, ATTR_BOLD), MARKER_LEFT_PADDING);
    } else {
      Array_Push(&prefixed.Spans, Span{indent, StyleDefault(), true});
    }
    for (u32 j = 0; j < logical.Data[i].Spans.Count; j++)
      Array_Push(&prefixed.Spans, logical.Data[i].Spans.Data[j]);
    // Markdown owns the inner continuation (list marker width, quote bar, code
    // indent); the icon indent goes in front of it rather than replacing it.
    // One Span still suffices because the markdown palette sets no background:
    // the leading blanks have no visible colour of their own.
    prefixed.Continuation = Span{indent, StyleDefault(), true};
    const SpanLine& row = logical.Data[i];
    if (row.HasContinuation && row.Continuation.Text.Length) {
      StrBuf joined = StrBuf_Make(&m->RowsMem, indent.Length + row.Continuation.Text.Length + 1);
      StrBuf_Append(&joined, indent);
      StrBuf_Append(&joined, row.Continuation.Text);
      prefixed.Continuation = Span{StrBuf_View(&joined), row.Continuation.St, true};
    }
    prefixed.HasContinuation = true;
    WrapAndAppend(m, &prefixed);
  }
}

void BuildTool(LineModel* m, const Entry* entry, bool expandPreview) {
  StrView marker = OutlineMarker(m);
  Style markerSt = entry->Tool.Error ? LStyle(m, 196, ATTR_BOLD)
                                     : LStyle(m, ASSISTANT_FG, ATTR_NONE);
  Style label = entry->Tool.Error ? LStyle(m, 196, ATTR_BOLD)
                                  : LStyle(m, 252, ATTR_BOLD);
  Style detail = entry->Tool.Error ? LStyle(m, 196, ATTR_NONE)
                                   : LStyle(m, 252, ATTR_NONE);
  StrView indent = IconIndent(&m->RowsMem, marker, MARKER_LEFT_PADDING);

  SpanLine logical;
  InitRow(&m->RowsMem, &logical, 7);
  PushIcon(&m->RowsMem, &logical.Spans, marker, markerSt,
           MARKER_LEFT_PADDING);
  if (entry->Tool.View.Label.Length) {
    Array_Push(&logical.Spans, Span{entry->Tool.View.Label, label});
    if (entry->Tool.View.Detail.Length) Array_Push(&logical.Spans, Span{SVL(" "), detail});
  }
  if (entry->Tool.View.Detail.Length)
    Array_Push(&logical.Spans, Span{BufView(entry->Tool.View.Detail), detail});
  if (entry->Tool.View.Outcome.Length) {
    Array_Push(&logical.Spans, Span{SVL("  "), detail});
    StrView outcome = BufView(entry->Tool.View.Outcome);
    u32 split = entry->Tool.View.OutcomeSplit;
    if (!entry->Tool.Error && split != 0 && split < outcome.Length) {
      // Diff stat: "+N " in green, "-M" in red.
      Array_Push(&logical.Spans,
                 Span{StrView{outcome.Data, split},
                      LStyle(m, DIFF_ADD_FG, ATTR_NONE)});
      Array_Push(
          &logical.Spans,
          Span{StrView{outcome.Data + split, outcome.Length - split},
               LStyle(m, DIFF_DEL_FG, ATTR_NONE)});
    } else {
      Array_Push(&logical.Spans,
                 Span{outcome,
                      entry->Tool.Error ? LStyle(m, 196, ATTR_NONE)
                                        : LStyle(m, 244, ATTR_NONE)});
    }
  }
  logical.Continuation = Span{indent, StyleDefault(), true};
  logical.HasContinuation = true;
  WrapAndAppend(m, &logical);

  if (expandPreview && entry->Tool.Done && entry->Tool.Preview.Length) {
    AppendPlain(m, BufView(entry->Tool.Preview),
                entry->Tool.Error ? LStyle(m, 196, ATTR_NONE)
                                  : LStyle(m, 244, ATTR_DIM),
                indent, indent);
  }
}

void BuildPlainTurn(LineModel* m, const Entry* entry, StrView marker, Style markerStyle,
                    Style textStyle, u32 markerLeftPadding = 0) {
  StrView text = BufView(entry->Text.Content);
  StrView indent = IconIndent(&m->RowsMem, marker, markerLeftPadding);
  u32 i = 0;
  bool firstLine = true;
  while (i <= text.Length) {
    u32 start = i;
    while (i < text.Length && text.Data[i] != '\n') i++;
    StrView line{text.Data + start, i - start};
    bool atEnd = i >= text.Length;
    i++;

    SpanLine logical;
    InitRow(&m->RowsMem, &logical, 3);
    if (firstLine) {
      PushIcon(&m->RowsMem, &logical.Spans, marker, markerStyle,
               markerLeftPadding);
    } else {
      Array_Push(&logical.Spans, Span{indent, textStyle, true});
    }
    if (line.Length) Array_Push(&logical.Spans, Span{line, textStyle});
    logical.Continuation = Span{indent, textStyle, true};
    logical.HasContinuation = true;
    WrapAndAppend(m, &logical);
    firstLine = false;
    if (atEnd) break;
  }
}

void BuildEntryRows(LineModel* m, u32 idx, u32 expandedStart) {
  Entry* entry = &m->Entries.Data[idx];
  if (!EntryVisible(entry)) return;
  const Entry* previous = PreviousVisible(m, idx);
  if (NeedsSeparator(previous, entry)) AddBlank(m);

  u32 rowStart = m->Lines.Count;
  bool previewHidden = false;
  switch (entry->Kind) {
    case EntryKind::User:
      BuildUser(m, entry);
      break;
    case EntryKind::Assistant:
      BuildAssistant(m, entry);
      break;
    case EntryKind::Tool: {
      bool hasPreview = entry->Tool.Done && entry->Tool.Preview.Length != 0;
      bool showPreview = !entry->Collapsed && hasPreview && idx >= expandedStart;
      BuildTool(m, entry, showPreview);
      previewHidden = hasPreview && !showPreview;
      break;
    }
    case EntryKind::Info:
      BuildPlainTurn(m, entry, FilledMarker(m),
                     LStyle(m, ACCENT_FG, ATTR_BOLD),
                     LStyle(m, 244, ATTR_NONE), MARKER_LEFT_PADDING);
      break;
    case EntryKind::Error:
      BuildPlainTurn(m, entry, SVL("!"), LStyle(m, 196, ATTR_BOLD),
                     LStyle(m, 196, ATTR_BOLD));
      break;
  }

  u32 rowCount = m->Lines.Count - rowStart;
  bool hidden = previewHidden;
  if (entry->Collapsed && rowCount > 1) {
    hidden = true;
    m->Lines.Count = rowStart + 1;
    rowCount = 1;
  }
  for (u32 i = 0; i < rowCount; i++) m->Lines.Data[rowStart + i].EntryId = entry->Id;
  if (rowCount != 0) {
    m->Lines.Data[rowStart].IsEntryStart = true;
    if (hidden) {
      m->Lines.Data[rowStart].HasHiddenContent = true;
      m->Lines.Data[rowStart].Hidden =
          Span{SVL("\xe2\x80\xa6"), LStyle(m, 244, ATTR_DIM)};
    }
  }
}

void CropDisplayToCap(LineModel* m) {
  if (m->Lines.Count <= m->Cap) {
    if (m->HasAssistant) m->AssistantRowOffset = 0;
    return;
  }
  u32 drop = m->Lines.Count - m->Cap;
  u32 assistantStart = m->AssistantStart;
  memmove(m->Lines.Data, m->Lines.Data + drop, m->Cap * sizeof(SpanLine));
  m->Lines.Count = m->Cap;
  if (m->HasAssistant) {
    m->AssistantRowOffset = drop > assistantStart ? drop - assistantStart : 0;
    m->AssistantStart = assistantStart > drop ? assistantStart - drop : 0;
  }
}

void AdvanceDisplayRevision(LineModel* m) {
  m->DisplayRevision++;
  if (m->DisplayRevision == 0) m->DisplayRevision++;
}

u32 PreviewRowCount(LineModel* m, const Entry* entry) {
  usize mark = m->RowsMem.Used;
  Array<SpanLine> rows = Array_Make<SpanLine>(&m->RowsMem, 8);
  StrView indent =
      IconIndent(&m->RowsMem, OutlineMarker(m), MARKER_LEFT_PADDING);
  AppendPlainRows(&m->RowsMem, &rows, m->Width, BufView(entry->Tool.Preview),
                  LStyle(m, 244, ATTR_DIM), indent, indent);
  u32 count = rows.Count;
  ArenaRestore(&m->RowsMem, mark);
  return count;
}

u32 ExpandedStart(LineModel* m) {
  u32 budget = m->Cap;
  u32 start = m->Entries.Count;
  for (u32 i = m->Entries.Count; i > 0; i--) {
    const Entry* entry = &m->Entries.Data[i - 1];
    if (entry->Kind != EntryKind::Tool || entry->Collapsed || !entry->Tool.Done ||
        entry->Tool.Preview.Length == 0)
      continue;
    u32 cost = PreviewRowCount(m, entry);
    if (cost > budget) break;
    budget -= cost;
    start = i - 1;
  }
  return start;
}

void BuildRows(LineModel* m, u32 stopAfter) {
  u32 expandedStart = ExpandedStart(m);
  for (u32 i = 0; i < m->Entries.Count; i++) {
    if (i == m->Entries.Count - 1 && m->HasAssistant) {
      m->AssistantStart = m->Lines.Count;
      m->AssistantRowsMark = m->RowsMem.Used;
    }
    BuildEntryRows(m, i, expandedStart);
    if (m->Lines.Count > stopAfter) break;
  }
}

void RebuildAll(LineModel* m, bool crop) {
  ArenaReset(&m->RowsMem);
  m->Lines.Count = 0;
  BuildRows(m, 0xffffffffu);
  if (crop) CropDisplayToCap(m);
}

bool TrimToCap(LineModel* m) {
  bool removedEntries = false;
  RebuildAll(m, false);
  while (m->Lines.Count > m->Cap && m->Entries.Count > 1) {
    memmove(m->Entries.Data, m->Entries.Data + 1, (m->Entries.Count - 1) * sizeof(Entry));
    m->Entries.Count--;
    removedEntries = true;
    RebuildAll(m, false);
  }
  CropDisplayToCap(m);
  return removedEntries;
}

}  // namespace

namespace {

StrView PrefixWithinCells(StrView text, u16 room, u16* width) {
  u32 i = 0;
  u16 cells = 0;
  while (i < text.Length) {
    u32 next = i;
    u32 cp = Utf8Decode(text, &next);
    u32 charWidth = CharCellWidth(cp);
    if ((u32)cells + charWidth > room) break;
    cells = (u16)(cells + charWidth);
    i = next;
  }
  *width = cells;
  return StrView{text.Data, i};
}

void AppendSpaces(StrBuf* out, u16 count) {
  while (count--) StrBuf_AppendChar(out, ' ');
}

u16 SnapDisplayBoundary(const DisplayRowLayout* layout, u16 column, bool towardEnd) {
  DisplayRowIterator iterator = {};
  DisplayRowRun run;
  while (LineModel_NextDisplayRowRun(layout, &iterator, &run)) {
    u32 i = 0;
    u16 runColumn = run.Column;
    while (i < run.Text.Length) {
      u32 next = i;
      u32 cp = Utf8Decode(run.Text, &next);
      u16 width = (u16)CharCellWidth(cp);
      u16 end = (u16)(runColumn + width);
      if (column > runColumn && column < end) return towardEnd ? end : runColumn;
      runColumn = end;
      i = next;
    }
  }
  return column;
}

}  // namespace

namespace {

void WrapSpanLine(Arena* a, const SpanLine* in, u16 width, Array<SpanLine>* out) {
  if (width < 1) width = 1;
  u32 rightWidth = in->HasRight ? LineTextWidth(in->Right.Text) : 0;
  bool showRight = in->HasRight && rightWidth + 3u < width;

  SpanLine current;
  u16 col = 0;
  u16 limit = width;
  bool first = true;
  StartWrappedRow(a, in, &current, first, showRight, rightWidth, width, &col, &limit);

  for (u32 s = 0; s < in->Spans.Count; s++) {
    Span span = in->Spans.Data[s];
    StrView text = span.Text;
    u32 i = 0;
    while (i < text.Length) {
      bool isSpace = text.Data[i] == ' ';
      u32 start = i;
      while (i < text.Length && (text.Data[i] == ' ') == isSpace) i++;
      StrView token{text.Data + start, i - start};

      if (isSpace) {
        if (col == 0 && !span.KeepLeading) continue;
        if (col >= limit) {
          Array_Push(out, current);
          first = false;
          StartWrappedRow(a, in, &current, first, showRight, rightWidth, width, &col, &limit);
          continue;
        }
        u32 room = (u32)limit - col;
        u32 take = token.Length < room ? token.Length : room;
        Array_Push(&current.Spans, Span{StrView{token.Data, take}, span.St});
        col = (u16)(col + take);
        continue;
      }

      u32 wordWidth = LineTextWidth(token);
      if (col > 0 && (u32)col + wordWidth > limit) {
        Array_Push(out, current);
        first = false;
        StartWrappedRow(a, in, &current, first, showRight, rightWidth, width, &col, &limit);
      }
      if ((u32)col + wordWidth <= limit) {
        Array_Push(&current.Spans, Span{token, span.St});
        col = (u16)(col + wordWidth);
      } else {
        u32 j = 0;
        while (j < token.Length) {
          u32 segmentStart = j;
          u16 segmentWidth = 0;
          while (j < token.Length) {
            u32 next = j;
            u32 cp = Utf8Decode(token, &next);
            u32 charWidth = CharCellWidth(cp);
            bool fresh = col == 0 && segmentWidth == 0;
            if (!fresh && (u32)col + segmentWidth + charWidth > limit) {
              if (segmentWidth == 0) {
                current.Spans.Count = 0;
                col = 0;
                continue;
              }
              break;
            }
            segmentWidth = (u16)(segmentWidth + charWidth);
            j = next;
            if ((u32)col + segmentWidth >= limit) break;
          }
          Array_Push(&current.Spans,
                     Span{StrView{token.Data + segmentStart, j - segmentStart}, span.St});
          col = (u16)(col + segmentWidth);
          if (j < token.Length) {
            Array_Push(out, current);
            first = false;
            StartWrappedRow(a, in, &current, first, showRight, rightWidth, width, &col, &limit);
          }
        }
      }
    }
  }
  Array_Push(out, current);
}

}  // namespace

void LineModel_Init(LineModel* m, Arena* mem, u16 width, u32 cap,
                    bool color, bool rounded) {
  m->Mem = mem;
  m->RowsMem = ArenaCreate(LINE_ROWS_RESERVE);
  m->Entries = Array_Make<Entry>(mem, 16);
  m->Lines = Array_Make<SpanLine>(mem, 128);
  m->Width = width ? width : 80;
  m->Cap = cap ? cap : 5000;
  m->AssistantStart = 0;
  m->AssistantRowOffset = 0;
  m->AssistantRowsMark = 0;
  m->NextEntryId = 1;
  m->DisplayRevision = 1;
  m->HasAssistant = false;
  m->Color = color;
  m->Rounded = rounded;
}

void LineModel_Release(LineModel* m) { ArenaRelease(&m->RowsMem); }

u32 LineModel_Count(const LineModel* m) { return m->Lines.Count; }

bool LineModel_FitsAtWidth(LineModel* m, u16 width, u32 maxRows) {
  if (maxRows >= m->Cap) return true;
  if (width < 1) width = 1;
  if (width == m->Width) return m->Lines.Count <= maxRows;

  usize rowsMark = m->RowsMem.Used;
  u16 savedWidth = m->Width;
  Array<SpanLine> savedLines = m->Lines;
  u32 savedAssistantStart = m->AssistantStart;
  usize savedAssistantRowsMark = m->AssistantRowsMark;

  m->Width = width;
  m->Lines = Array_Make<SpanLine>(&m->RowsMem, 32);
  BuildRows(m, maxRows);
  bool fits = m->Lines.Count <= maxRows;

  ArenaRestore(&m->RowsMem, rowsMark);
  m->Width = savedWidth;
  m->Lines = savedLines;
  m->AssistantStart = savedAssistantStart;
  m->AssistantRowsMark = savedAssistantRowsMark;
  return fits;
}

const SpanLine* LineModel_At(const LineModel* m, u32 i) {
  return i < m->Lines.Count ? &m->Lines.Data[i] : nullptr;
}

u64 LineModel_RowEntryId(const LineModel* m, u32 row) {
  const SpanLine* line = LineModel_At(m, row);
  return line ? line->EntryId : 0;
}

u64 LineModel_DisplayRevision(const LineModel* m) { return m->DisplayRevision; }

DisplayRowLayout LineModel_LayoutDisplayRow(const SpanLine* line, u16 columns) {
  DisplayRowLayout layout = {};
  layout.Line = line;
  layout.Columns = columns;
  layout.LeftLimit = columns;
  layout.HiddenColumn = columns;
  layout.RightColumn = columns;
  if (!line || columns == 0) return layout;

  u32 rightWidth = line->HasRight ? LineTextWidth(line->Right.Text) : 0;
  bool drawRight = line->HasRight && rightWidth < columns;
  bool drawHidden = line->HasHiddenContent;
  if (drawHidden && drawRight && rightWidth + 2u >= columns) drawRight = false;
  if (drawRight) {
    layout.RightColumn = (u16)(columns - rightWidth);
    layout.LeftLimit = (u16)(layout.RightColumn - 1u);
  }
  if (drawHidden) {
    layout.HiddenColumn = drawRight ? (u16)(layout.RightColumn - 2u) : (u16)(columns - 1u);
    if (layout.HiddenColumn < layout.LeftLimit) layout.LeftLimit = layout.HiddenColumn;
  }

  DisplayRowIterator iterator = {};
  DisplayRowRun run;
  while (LineModel_NextDisplayRowRun(&layout, &iterator, &run)) {
    u16 end = (u16)(run.Column + run.CellWidth);
    if (layout.ContentEnd < end) layout.ContentEnd = end;
  }
  return layout;
}

bool LineModel_NextDisplayRowRun(const DisplayRowLayout* layout, DisplayRowIterator* iterator,
                                 DisplayRowRun* run) {
  if (!layout || !layout->Line || !iterator || !run) return false;
  const SpanLine* line = layout->Line;
  while (iterator->SpanIndex < line->Spans.Count) {
    if (iterator->Column >= layout->LeftLimit) {
      iterator->SpanIndex = line->Spans.Count;
      break;
    }
    Span span = line->Spans.Data[iterator->SpanIndex++];
    u16 width = 0;
    StrView text =
        PrefixWithinCells(span.Text, (u16)(layout->LeftLimit - iterator->Column), &width);
    if (text.Length == 0) continue;
    *run = DisplayRowRun{text, span.St, iterator->Column, width};
    iterator->Column = (u16)(iterator->Column + width);
    return true;
  }
  if (iterator->SpanIndex == line->Spans.Count) {
    iterator->SpanIndex++;
    if (layout->HiddenColumn < layout->Columns) {
      StrView text =
          line->Hidden.Text.Length ? line->Hidden.Text : SVL("\xe2\x80\xa6");
      Style style =
          line->Hidden.Text.Length
              ? line->Hidden.St
              : StyleMake(244, COL_DEFAULT, ATTR_DIM);
      *run = DisplayRowRun{text, style, layout->HiddenColumn, 1};
      return true;
    }
  }
  if (iterator->SpanIndex == line->Spans.Count + 1u) {
    iterator->SpanIndex++;
    if (layout->RightColumn < layout->Columns) {
      *run = DisplayRowRun{line->Right.Text, line->Right.St, layout->RightColumn,
                           (u16)(layout->Columns - layout->RightColumn)};
      return true;
    }
  }
  return false;
}

DisplayRowRange LineModel_NormalizeDisplayRowRange(const DisplayRowLayout* layout, u16 anchor,
                                                   u16 focus) {
  DisplayRowRange range = {};
  if (!layout) return range;
  if (anchor > layout->Columns) anchor = layout->Columns;
  if (focus > layout->Columns) focus = layout->Columns;
  range.Start = anchor < focus ? anchor : focus;
  range.End = anchor < focus ? focus : anchor;
  if (range.Start == range.End) return range;
  range.Start = SnapDisplayBoundary(layout, range.Start, false);
  range.End = SnapDisplayBoundary(layout, range.End, true);
  return range;
}

namespace {

void AccumulateDisplayBytes(StrBuf* out, u64* hash, StrView text) {
  if (out) StrBuf_Append(out, text);
  if (!hash) return;
  for (u32 i = 0; i < text.Length; i++) {
    *hash ^= (u8)text.Data[i];
    *hash *= 1099511628211ull;
  }
}

void AccumulateDisplaySpaces(StrBuf* out, u64* hash, u16 count) {
  if (out) AppendSpaces(out, count);
  if (!hash) return;
  while (count--) {
    *hash ^= (u8)' ';
    *hash *= 1099511628211ull;
  }
}

void AccumulateDisplayRow(const DisplayRowLayout* layout, DisplayRowRange range,
                          StrBuf* out, u64* hash) {
  u16 outputColumn = range.Start;
  DisplayRowIterator iterator = {};
  DisplayRowRun run;
  while (LineModel_NextDisplayRowRun(layout, &iterator, &run)) {
    if (run.Column >= range.End) break;
    u32 i = 0;
    u16 column = run.Column;
    while (i < run.Text.Length) {
      u32 start = i;
      u32 cp = Utf8Decode(run.Text, &i);
      u16 width = (u16)CharCellWidth(cp);
      u16 end = (u16)(column + width);
      if (end <= range.Start || end <= outputColumn) {
        column = end;
        continue;
      }
      if (column >= range.End) break;
      if (column > outputColumn)
        AccumulateDisplaySpaces(
            out, hash, (u16)((column < range.End ? column : range.End) - outputColumn));
      if (column < range.End) {
        AccumulateDisplayBytes(out, hash, StrView{run.Text.Data + start, i - start});
        outputColumn = end;
      }
      column = end;
    }
  }
  if (outputColumn < range.End)
    AccumulateDisplaySpaces(out, hash, (u16)(range.End - outputColumn));
}

}  // namespace

StrView LineModel_ExtractDisplayRow(Arena* arena, const DisplayRowLayout* layout, u16 anchor,
                                    u16 focus) {
  if (!arena || !layout) return StrView{};
  DisplayRowRange range = LineModel_NormalizeDisplayRowRange(layout, anchor, focus);
  if (range.Start == range.End) return StrView{"", 0};

  StrBuf out = StrBuf_Make(arena, (u32)(range.End - range.Start) * 4u + 1u);
  AccumulateDisplayRow(layout, range, &out, nullptr);
  return StrBuf_View(&out);
}

u64 LineModel_HashDisplayRow(const DisplayRowLayout* layout, u16 anchor, u16 focus) {
  if (!layout) return 0;
  DisplayRowRange range = LineModel_NormalizeDisplayRowRange(layout, anchor, focus);
  u64 hash = 1469598103934665603ull;
  AccumulateDisplayRow(layout, range, nullptr, &hash);
  return hash;
}

namespace {

u64 HashDisplayValue(u64 hash, u64 value) {
  for (u32 i = 0; i < 8; i++) {
    hash ^= (u8)(value >> (i * 8));
    hash *= 1099511628211ull;
  }
  return hash;
}

u64 DisplayRowPrefixHash(const SpanLine* line, u16 columns, u16 limit) {
  DisplayRowLayout layout = LineModel_LayoutDisplayRow(line, columns);
  u64 hash = 1469598103934665603ull;
  DisplayRowIterator iterator = {};
  DisplayRowRun run;
  while (LineModel_NextDisplayRowRun(&layout, &iterator, &run)) {
    if (run.Column >= limit) break;
    u16 width = 0;
    StrView text = PrefixWithinCells(run.Text, (u16)(limit - run.Column), &width);
    if (text.Length == 0) continue;
    hash = HashDisplayValue(hash, run.Column);
    hash = HashDisplayValue(hash, width);
    for (u32 i = 0; i < text.Length; i++) {
      hash ^= (u8)text.Data[i];
      hash *= 1099511628211ull;
    }
  }
  return hash;
}

struct AssistantPrefixFingerprint {
  u32 RowCount;
  u16 LastEnd;
  u64 FixedRows;
  u64 LastRow;
};

AssistantPrefixFingerprint CaptureAssistantPrefix(const LineModel* m) {
  AssistantPrefixFingerprint result = {};
  result.FixedRows = 1469598103934665603ull;
  if (m->Lines.Count <= m->AssistantStart) return result;
  result.RowCount = m->Lines.Count - m->AssistantStart;
  for (u32 i = 0; i + 1 < result.RowCount; i++) {
    const SpanLine* line = &m->Lines.Data[m->AssistantStart + i];
    DisplayRowLayout layout = LineModel_LayoutDisplayRow(line, m->Width);
    result.FixedRows = HashDisplayValue(result.FixedRows, layout.ContentEnd);
    result.FixedRows =
        HashDisplayValue(result.FixedRows,
                         DisplayRowPrefixHash(line, m->Width, layout.ContentEnd));
  }
  const SpanLine* last = &m->Lines.Data[m->Lines.Count - 1u];
  DisplayRowLayout layout = LineModel_LayoutDisplayRow(last, m->Width);
  result.LastEnd = layout.ContentEnd;
  result.LastRow = DisplayRowPrefixHash(last, m->Width, result.LastEnd);
  return result;
}

bool AssistantPrefixIsStable(const LineModel* m,
                             const AssistantPrefixFingerprint& previous) {
  if (previous.RowCount == 0) return true;
  if (m->Lines.Count < m->AssistantStart + previous.RowCount) return false;
  u64 fixedRows = 1469598103934665603ull;
  for (u32 i = 0; i + 1 < previous.RowCount; i++) {
    const SpanLine* line = &m->Lines.Data[m->AssistantStart + i];
    DisplayRowLayout layout = LineModel_LayoutDisplayRow(line, m->Width);
    fixedRows = HashDisplayValue(fixedRows, layout.ContentEnd);
    fixedRows =
        HashDisplayValue(fixedRows,
                         DisplayRowPrefixHash(line, m->Width, layout.ContentEnd));
  }
  if (fixedRows != previous.FixedRows) return false;
  const SpanLine* last = &m->Lines.Data[m->AssistantStart + previous.RowCount - 1u];
  DisplayRowLayout layout = LineModel_LayoutDisplayRow(last, m->Width);
  return layout.ContentEnd >= previous.LastEnd &&
         DisplayRowPrefixHash(last, m->Width, previous.LastEnd) == previous.LastRow;
}

}  // namespace

bool LineModel_FindEntryRows(const LineModel* m, u64 entryId, u32* firstRow, u32* rowCount) {
  if (entryId == 0) return false;
  for (u32 i = 0; i < m->Lines.Count; i++) {
    if (m->Lines.Data[i].EntryId != entryId) continue;
    u32 end = i + 1;
    while (end < m->Lines.Count && m->Lines.Data[end].EntryId == entryId) end++;
    if (firstRow) *firstRow = i;
    if (rowCount) *rowCount = end - i;
    return true;
  }
  return false;
}

bool LineModel_IsEntryCollapsed(const LineModel* m, u64 entryId) {
  for (u32 i = 0; i < m->Entries.Count; i++) {
    const Entry* entry = &m->Entries.Data[i];
    if (entry->Id == entryId) return entry->Collapsed;
  }
  return false;
}

void LineModel_SetWidth(LineModel* m, u16 width) {
  if (width < 1) width = 1;
  if (width == m->Width) return;
  m->Width = width;
  TrimToCap(m);
  AdvanceDisplayRevision(m);
}

bool LineModel_SetRounded(LineModel* m, bool rounded) {
  if (m->Rounded == rounded) return false;
  m->Rounded = rounded;
  RebuildAll(m, true);
  AdvanceDisplayRevision(m);
  return true;
}

bool LineModel_ToggleEntry(LineModel* m, u64 entryId) {
  for (u32 i = 0; i < m->Entries.Count; i++) {
    Entry* entry = &m->Entries.Data[i];
    if (entry->Id != entryId) continue;
    entry->Collapsed = !entry->Collapsed;
    RebuildAll(m, true);
    AdvanceDisplayRevision(m);
    return true;
  }
  return false;
}

bool LineModel_ToggleAllTools(LineModel* m) {
  bool anyTool = false;
  bool expandAll = false;
  for (u32 i = 0; i < m->Entries.Count; i++) {
    const Entry* entry = &m->Entries.Data[i];
    if (entry->Kind != EntryKind::Tool) continue;
    anyTool = true;
    if (entry->Collapsed) expandAll = true;
  }
  if (!anyTool) return false;

  bool collapsed = !expandAll;
  bool changed = false;
  for (u32 i = 0; i < m->Entries.Count; i++) {
    Entry* entry = &m->Entries.Data[i];
    if (entry->Kind != EntryKind::Tool || entry->Collapsed == collapsed) continue;
    entry->Collapsed = collapsed;
    changed = true;
  }
  if (changed) {
    RebuildAll(m, true);
    AdvanceDisplayRevision(m);
  }
  return changed;
}

void LineModel_BeginAssistant(LineModel* m) {
  FFI(m->HasAssistant);
  Entry entry = {};
  entry.Id = m->NextEntryId++;
  FFI(entry.Id == 0);
  entry.Kind = EntryKind::Assistant;
  entry.Text.Content = StrBuf_Make(m->Mem, 64);
  Array_Push(&m->Entries, entry);
  m->HasAssistant = true;
  m->AssistantStart = m->Lines.Count;
  m->AssistantRowOffset = 0;
  m->AssistantRowsMark = m->RowsMem.Used;
}

void LineModel_AppendAssistant(LineModel* m, StrView delta) {
  FFI(!m->HasAssistant || m->Entries.Count == 0);
  if (delta.Length == 0) return;
  u32 previousAssistantStart = m->AssistantStart;
  u32 previousAssistantRowOffset = m->AssistantRowOffset;
  AssistantPrefixFingerprint previous = CaptureAssistantPrefix(m);
  Entry* entry = &m->Entries.Data[m->Entries.Count - 1];
  StrBuf_Append(&entry->Text.Content, delta);
  ArenaRestore(&m->RowsMem, m->AssistantRowsMark);
  m->Lines.Count = m->AssistantStart;
  BuildEntryRows(m, m->Entries.Count - 1, m->Entries.Count);
  CropDisplayToCap(m);
  if (m->AssistantStart != previousAssistantStart ||
      m->AssistantRowOffset != previousAssistantRowOffset ||
      !AssistantPrefixIsStable(m, previous))
    AdvanceDisplayRevision(m);
}

void LineModel_EndAssistant(LineModel* m) {
  FFI(!m->HasAssistant);
  m->HasAssistant = false;
  m->AssistantRowOffset = 0;
  if (TrimToCap(m)) AdvanceDisplayRevision(m);
}

void LineModel_AddNotice(LineModel* m, EntryKind kind, StrView text) {
  FFI(kind != EntryKind::Info && kind != EntryKind::Error);
  FFI(m->HasAssistant);
  Entry entry = {};
  entry.Id = m->NextEntryId++;
  FFI(entry.Id == 0);
  entry.Kind = kind;
  entry.Text.Content = StrBuf_Make(m->Mem, text.Length + 1);
  StrBuf_Append(&entry.Text.Content, text);
  Array_Push(&m->Entries, entry);
  TrimToCap(m);
  AdvanceDisplayRevision(m);
}

void LineModel_AddUser(LineModel* m, StrView text, u64 timestampMs) {
  FFI(m->HasAssistant);
  Entry entry = {};
  entry.Id = m->NextEntryId++;
  FFI(entry.Id == 0);
  entry.Kind = EntryKind::User;
  entry.Text.Content = StrBuf_Make(m->Mem, text.Length + 1);
  StrBuf_Append(&entry.Text.Content, text);
  entry.Text.Timestamp = FormatTimestamp(m->Mem, timestampMs);
  Array_Push(&m->Entries, entry);
  TrimToCap(m);
  AdvanceDisplayRevision(m);
}

void LineModel_AddTool(LineModel* m, StrView toolId, StrView name, StrView argsJson) {
  FFI(m->HasAssistant);
  Entry entry = {};
  entry.Id = m->NextEntryId++;
  FFI(entry.Id == 0);
  entry.Kind = EntryKind::Tool;
  entry.Collapsed = true;
  entry.Tool.Id = StrView_Dup(m->Mem, toolId);
  entry.Tool.Preview = StrBuf_Make(m->Mem, 256);
  ToolView_Init(m->Mem, &entry.Tool.View, name, argsJson);
  Array_Push(&m->Entries, entry);
  TrimToCap(m);
  AdvanceDisplayRevision(m);
}

bool LineModel_UpdateTool(LineModel* m, StrView toolId, StrView preview) {
  for (u32 i = m->Entries.Count; i > 0; i--) {
    Entry* entry = &m->Entries.Data[i - 1];
    if (entry->Kind != EntryKind::Tool || entry->Tool.Done) continue;
    if (!StrView_Eq(entry->Tool.Id, toolId)) continue;
    StrBuf_Clear(&entry->Tool.Preview);
    StrBuf_Append(&entry->Tool.Preview, preview);
    RebuildAll(m, true);
    AdvanceDisplayRevision(m);
    return true;
  }
  return false;
}

bool LineModel_FinishTool(LineModel* m, StrView toolId, StrView result, StrView preview, bool isError) {
  for (u32 i = m->Entries.Count; i > 0; i--) {
    Entry* entry = &m->Entries.Data[i - 1];
    if (entry->Kind != EntryKind::Tool) continue;
    if (!StrView_Eq(entry->Tool.Id, toolId)) continue;
    if (entry->Tool.Done) return true;
    entry->Tool.Done = true;
    entry->Tool.Error = isError;
    StrBuf_Clear(&entry->Tool.Preview);
    StrBuf_Append(&entry->Tool.Preview, preview);
    ToolView_Finish(&entry->Tool.View, result, isError);
    RebuildAll(m, true);
    AdvanceDisplayRevision(m);
    return true;
  }
  return false;
}
