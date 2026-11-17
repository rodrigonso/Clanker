#include "tui/linemodel.h"

#include "base/sysinfo.h"

#include <string.h>
#include <time.h>

#include "tui/markdown.h"

namespace {

const usize LINE_ROWS_RESERVE = (usize)64 * 1024 * 1024;
const u32 NO_ENTRY = 0xffffffffu;
const u32 ICON_FIELD_CELLS = 2;
const u32 MARKER_LEFT_PADDING = 1;

// Bullet color scheme: the system accent for user messages and white for
// assistant responses. Tool calls show an outlined marker while pending
// (animated in the accent while running here), then a green check or a red
// cross.
const u8 ASSISTANT_FG = 255;
// Diff stats and diff lines are the one non-error place red appears: it is
// paired with green and reads as "lines removed", not as a failure. Errors
// still own bold 196. The same green marks a successful tool call.
const u8 DIFF_ADD_FG = 71;
const u8 DIFF_DEL_FG = 167;
// A running call's expanded preview follows its output instead of growing.
const u32 LIVE_TAIL_ROWS = 5;
StrView FilledMarker(const LineModel* m) {
  return m->Rounded ? SVL("\xe2\x97\x8f") : SVL("\xe2\x97\xbc");
}
StrView OutlineMarker(const LineModel* m) {
  return m->Rounded ? SVL("\xe2\x97\x8b") : SVL("\xe2\x97\xbb");
}
const StrView CHECK_MARKER = SVL("\xe2\x9c\x93");
const StrView CROSS_MARKER = SVL("\xe2\x9c\x97");
// Notices are the app talking, not a participant: a dot dimmer than the grey
// text beside it, so it recedes behind the turn markers.
const u8 NOTICE_FG = 242;
const u8 MUTED_FG = 244;
const u8 RULE_FG = 238;

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
              u32 leftPadding = 0, bool live = false) {
  if (leftPadding)
    Array_Push(spans, Span{Spaces(arena, leftPadding), StyleDefault(), true});
  Array_Push(spans, Span{marker, style, false, live});
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
  Array_Push(&m->Lines, SpanLine{});
}

void WrapAndAppend(LineModel* m, SpanLine* line) {
  WrapSpanLine(&m->RowsMem, line, m->Width, &m->Lines);
}

void AppendLogicalRow(Arena* rowsMem, Array<SpanLine>* out, u16 width, StrView line,
                      Style style, StrView prefix, StrView continuation) {
  SpanLine logical;
  InitRow(rowsMem, &logical, 2);
  if (prefix.Length) Array_Push(&logical.Spans, Span{prefix, style, true});
  if (line.Length) Array_Push(&logical.Spans, Span{line, style});
  if (continuation.Length) {
    logical.Continuation = Span{continuation, style, true};
    logical.HasContinuation = true;
  }
  WrapSpanLine(rowsMem, &logical, width, out);
}

// Edit results open with a sentence the row header already conveys.
StrView DiffBody(StrView text) {
  if (!StrView_StartsWith(text, SVL("Successfully replaced "))) return text;
  for (u32 i = 0; i + 1 < text.Length; i++) {
    if (text.Data[i] == '\n' && text.Data[i + 1] == '\n')
      return StrView{text.Data + i + 2, text.Length - i - 2};
  }
  return text;
}

// Diff_Make lines are "<+|-| ><lineNo> <text>"; the digit check keeps prose
// that merely starts with '-' neutral, as CountDiffLines does.
Style PreviewLineStyle(const LineModel* m, StrView line, bool diff, Style base) {
  if (!diff || line.Length < 2 || line.Data[1] < '0' || line.Data[1] > '9') return base;
  if (line.Data[0] == '+') return LStyle(m, DIFF_ADD_FG, ATTR_NONE);
  if (line.Data[0] == '-') return LStyle(m, DIFF_DEL_FG, ATTR_NONE);
  return base;
}

// Both the renderer and the expanded-preview budget size previews through
// here, so the two always agree on the row count.
void AppendToolPreviewRows(const LineModel* m, Arena* rowsMem, Array<SpanLine>* out,
                           const Entry* entry, StrView indent) {
  const ToolEntry& tool = entry->Tool;
  StrView text = BufView(tool.Preview);
  bool diff = tool.Done && !tool.Error && tool.View.Kind == ToolViewKind::Edit;
  if (diff) text = DiffBody(text);
  while (text.Length &&
         (text.Data[text.Length - 1] == '\n' || text.Data[text.Length - 1] == '\r'))
    text.Length--;
  Style base = tool.Error ? LStyle(m, 196, ATTR_NONE) : LStyle(m, MUTED_FG, ATTR_DIM);
  u32 first = out->Count;
  u32 i = 0;
  while (i < text.Length) {
    u32 start = i;
    while (i < text.Length && text.Data[i] != '\n') i++;
    StrView line{text.Data + start, i - start};
    AppendLogicalRow(rowsMem, out, m->Width, line, PreviewLineStyle(m, line, diff, base),
                     indent, indent);
    i++;
  }
  if (!tool.Done && out->Count - first > LIVE_TAIL_ROWS) {
    memmove(out->Data + first, out->Data + out->Count - LIVE_TAIL_ROWS,
            LIVE_TAIL_ROWS * sizeof(SpanLine));
    out->Count = first + LIVE_TAIL_ROWS;
  }
}

bool EntryVisible(const Entry* entry) {
  if (entry->Kind == EntryKind::Assistant) return entry->Text.Content.Length != 0;
  return true;
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
               LStyle(m, m->Accent, ATTR_BOLD), MARKER_LEFT_PADDING);
    } else {
      Array_Push(&logical.Spans, Span{indent, StyleDefault(), true});
    }
    if (line.Length) Array_Push(&logical.Spans, Span{line, StyleDefault()});
    logical.Continuation = Span{indent, StyleDefault(), true};
    logical.HasContinuation = true;
    if (firstLine && entry->Text.Timestamp.Length) {
      logical.Right =
          Span{entry->Text.Timestamp, LStyle(m, MUTED_FG, ATTR_NONE)};
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
                  MarkdownOpts_Make(contentWidth, m->Color, m->Accent));
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

// Running calls tick in whole seconds; finished ones keep tenths below ten.
StrView FormatDuration(Arena* arena, u64 ms, bool tenths) {
  StrBuf b = StrBuf_Make(arena, 16);
  u64 seconds = ms / 1000;
  if (tenths && seconds < 10) {
    StrBuf_AppendU64(&b, seconds);
    StrBuf_AppendChar(&b, '.');
    StrBuf_AppendU64(&b, (ms / 100) % 10);
    StrBuf_AppendChar(&b, 's');
  } else if (seconds < 60) {
    StrBuf_AppendU64(&b, seconds);
    StrBuf_AppendChar(&b, 's');
  } else {
    bool hours = seconds >= 3600;
    u64 major = hours ? seconds / 3600 : seconds / 60;
    u64 minor = hours ? (seconds / 60) % 60 : seconds % 60;
    StrBuf_AppendU64(&b, major);
    StrBuf_AppendChar(&b, hours ? 'h' : 'm');
    if (minor < 10) StrBuf_AppendChar(&b, '0');
    StrBuf_AppendU64(&b, minor);
    StrBuf_AppendChar(&b, hours ? 'm' : 's');
  }
  return StrBuf_View(&b);
}

// Sub-second calls show no timer, so quick reads stay uncluttered.
StrView ToolTimer(Arena* arena, const ToolEntry& tool) {
  if (tool.Live)
    return tool.ElapsedSeconds ? FormatDuration(arena, (u64)tool.ElapsedSeconds * 1000, false)
                               : StrView{};
  if (!tool.Done || !tool.StartMs || tool.EndMs < tool.StartMs + 1000) return StrView{};
  return FormatDuration(arena, tool.EndMs - tool.StartMs, true);
}

void BuildTool(LineModel* m, const Entry* entry, bool expandPreview) {
  const ToolEntry& tool = entry->Tool;
  StrView marker = OutlineMarker(m);
  Style markerSt = LStyle(m, ASSISTANT_FG, ATTR_NONE);
  if (tool.Done && tool.Error) {
    marker = CROSS_MARKER;
    markerSt = LStyle(m, 196, ATTR_BOLD);
  } else if (tool.Done) {
    marker = CHECK_MARKER;
    markerSt = LStyle(m, DIFF_ADD_FG, ATTR_NONE);
  } else if (tool.Live) {
    markerSt = LStyle(m, m->Accent, ATTR_NONE);
  }
  Style label = tool.Error ? LStyle(m, 196, ATTR_BOLD) : LStyle(m, 252, ATTR_BOLD);
  Style detail = tool.Error ? LStyle(m, 196, ATTR_NONE) : LStyle(m, 252, ATTR_NONE);
  StrView indent = IconIndent(&m->RowsMem, marker, MARKER_LEFT_PADDING);

  SpanLine logical;
  InitRow(&m->RowsMem, &logical, 7);
  PushIcon(&m->RowsMem, &logical.Spans, marker, markerSt, MARKER_LEFT_PADDING, tool.Live);
  if (tool.View.Label.Length) {
    Array_Push(&logical.Spans, Span{tool.View.Label, label});
    if (tool.View.Detail.Length) Array_Push(&logical.Spans, Span{SVL(" "), detail});
  }
  if (tool.View.Detail.Length)
    Array_Push(&logical.Spans, Span{BufView(tool.View.Detail), detail});
  if (tool.View.Outcome.Length) {
    Array_Push(&logical.Spans, Span{SVL("  "), detail});
    StrView outcome = BufView(tool.View.Outcome);
    u32 split = tool.View.OutcomeSplit;
    if (!tool.Error && split != 0 && split < outcome.Length) {
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
                      tool.Error ? LStyle(m, 196, ATTR_NONE)
                                 : LStyle(m, MUTED_FG, ATTR_NONE)});
    }
  }
  StrView timer = ToolTimer(&m->RowsMem, tool);
  if (timer.Length) {
    logical.Right = Span{timer, LStyle(m, MUTED_FG, ATTR_NONE)};
    logical.HasRight = true;
  }
  logical.Continuation = Span{indent, StyleDefault(), true};
  logical.HasContinuation = true;
  WrapAndAppend(m, &logical);

  if (expandPreview && tool.Preview.Length)
    AppendToolPreviewRows(m, &m->RowsMem, &m->Lines, entry, indent);
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

void BuildThinking(LineModel* m, const Entry* entry) {
  SpanLine logical;
  InitRow(&m->RowsMem, &logical, 4);
  PushIcon(&m->RowsMem, &logical.Spans, OutlineMarker(m), LStyle(m, m->Accent, ATTR_NONE),
           MARKER_LEFT_PADDING, true);
  Array_Push(&logical.Spans,
             Span{SVL("Thinking\xe2\x80\xa6"), LStyle(m, MUTED_FG, ATTR_NONE)});
  u32 seconds = entry->Thinking.ElapsedSeconds;
  if (seconds) {
    logical.Right = Span{FormatDuration(&m->RowsMem, (u64)seconds * 1000, false),
                         LStyle(m, MUTED_FG, ATTR_NONE)};
    logical.HasRight = true;
  }
  WrapAndAppend(m, &logical);
}

// pi's token abbreviation: 842, 1.2k, 12k, 1.2M, 12M.
void AppendTokenCount(StrBuf* out, u64 n) {
  if (n < 1000) {
    StrBuf_AppendU64(out, n);
    return;
  }
  bool millions = n >= 1000000;
  u64 unit = millions ? 1000000 : 1000;
  if (n < unit * 10) {
    u64 tenths = (n + unit / 20) / (unit / 10);
    StrBuf_AppendU64(out, tenths / 10);
    StrBuf_AppendChar(out, '.');
    StrBuf_AppendU64(out, tenths % 10);
  } else {
    StrBuf_AppendU64(out, (n + unit / 2) / unit);
  }
  StrBuf_AppendChar(out, millions ? 'M' : 'k');
}

void BuildFooter(LineModel* m, const Entry* entry) {
  const TurnStats& stats = entry->Footer;
  StrView duration = FormatDuration(&m->RowsMem, stats.DurationMs, true);
  StrBuf label = StrBuf_Make(&m->RowsMem, 96);
  StrBuf_Append(&label, duration);
  if (stats.Tools) {
    StrBuf_AppendCStr(&label, " \xc2\xb7 ");
    StrBuf_AppendU64(&label, stats.Tools);
    StrBuf_AppendCStr(&label, stats.Tools == 1 ? " tool" : " tools");
  }
  if (stats.InputTokens || stats.OutputTokens) {
    StrBuf_AppendCStr(&label, " \xc2\xb7 \xe2\x86\x91");
    AppendTokenCount(&label, stats.InputTokens);
    StrBuf_AppendCStr(&label, " \xe2\x86\x93");
    AppendTokenCount(&label, stats.OutputTokens);
  }
  StrView text = StrBuf_View(&label);
  Style rule = LStyle(m, RULE_FG, ATTR_NONE);

  SpanLine logical;
  InitRow(&m->RowsMem, &logical, 4);
  // The lead rule fills the icon field, so the label lines up with message text.
  Array_Push(&logical.Spans, Span{Spaces(&m->RowsMem, MARKER_LEFT_PADDING), StyleDefault(), true});
  Array_Push(&logical.Spans, Span{SVL("\xe2\x94\x80\xe2\x94\x80 "), rule, true});
  Array_Push(&logical.Spans, Span{text, LStyle(m, MUTED_FG, ATTR_NONE)});
  // One cell short of the edge, mirroring the left padding.
  u32 used = MARKER_LEFT_PADDING + ICON_FIELD_CELLS + 1u + LineTextWidth(text) + 1u;
  if (m->Width > used + 1u) {
    u32 fill = m->Width - used - 1u;
    StrBuf tail = StrBuf_Make(&m->RowsMem, 1 + fill * 3);
    StrBuf_AppendChar(&tail, ' ');
    for (u32 i = 0; i < fill; i++) StrBuf_AppendCStr(&tail, "\xe2\x94\x80");
    Array_Push(&logical.Spans, Span{StrBuf_View(&tail), rule});
  }
  WrapAndAppend(m, &logical);
}

void BuildEntryRows(LineModel* m, const Entry* entry, bool showPreview) {
  if (!EntryVisible(entry)) return;
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
      bool hasPreview = entry->Tool.Preview.Length != 0;
      BuildTool(m, entry, showPreview);
      previewHidden = hasPreview && !showPreview;
      break;
    }
    case EntryKind::Info:
      BuildPlainTurn(m, entry, FilledMarker(m), LStyle(m, NOTICE_FG, ATTR_NONE),
                     LStyle(m, MUTED_FG, ATTR_ITALIC), MARKER_LEFT_PADDING);
      break;
    case EntryKind::Error:
      BuildPlainTurn(m, entry, SVL("!"), LStyle(m, 196, ATTR_BOLD),
                     LStyle(m, 196, ATTR_BOLD));
      break;
    case EntryKind::Thinking:
      BuildThinking(m, entry);
      break;
    case EntryKind::Footer:
      BuildFooter(m, entry);
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
    // Footers and the thinking row are chrome, not turns to hover or toggle.
    m->Lines.Data[rowStart].IsEntryStart =
        entry->Kind != EntryKind::Footer && entry->Kind != EntryKind::Thinking;
    if (hidden) {
      m->Lines.Data[rowStart].HasHiddenContent = true;
      m->Lines.Data[rowStart].Hidden =
          Span{SVL("\xe2\x80\xa6"), LStyle(m, MUTED_FG, ATTR_DIM)};
    }
  }
}

void AdvanceDisplayRevision(LineModel* m) {
  m->DisplayRevision++;
  if (m->DisplayRevision == 0) m->DisplayRevision++;
}

u32 PreviewRowCount(LineModel* m, const Entry* entry) {
  ArenaReset(&m->LayoutMem);
  Array<SpanLine> rows = Array_Make<SpanLine>(&m->LayoutMem, 8);
  StrView indent =
      IconIndent(&m->LayoutMem, OutlineMarker(m), MARKER_LEFT_PADDING);
  AppendToolPreviewRows(m, &m->LayoutMem, &rows, entry, indent);
  return rows.Count;
}

u64 ExpandedEntryId(LineModel* m) {
  u32 budget = m->Cap;
  u64 start = ~(u64)0;
  for (u32 i = m->Entries.Count; i > 0; i--) {
    Entry* entry = &m->Entries.Data[i - 1];
    if (entry->Kind != EntryKind::Tool || entry->Collapsed || !entry->Tool.Done ||
        entry->Tool.Preview.Length == 0)
      continue;
    if (entry->PreviewRows == 0xffffffffu)
      entry->PreviewRows = PreviewRowCount(m, entry);
    u32 cost = entry->PreviewRows;
    if (cost > budget) break;
    budget -= cost;
    start = entry->Id;
  }
  return start;
}

Array<SpanLine> CopyRows(Arena* mem, const Array<SpanLine>& source) {
  Array<SpanLine> rows = Array_Make<SpanLine>(mem, source.Count);
  for (u32 i = 0; i < source.Count; i++) {
    SpanLine row = source.Data[i];
    row.Spans = Array_Make<Span>(mem, row.Spans.Count);
    for (u32 s = 0; s < source.Data[i].Spans.Count; s++) {
      Span span = source.Data[i].Spans.Data[s];
      span.Text = StrView_Dup(mem, span.Text);
      Array_Push(&row.Spans, span);
    }
    if (row.HasRight) row.Right.Text = StrView_Dup(mem, row.Right.Text);
    if (row.HasContinuation)
      row.Continuation.Text = StrView_Dup(mem, row.Continuation.Text);
    if (row.HasHiddenContent) row.Hidden.Text = StrView_Dup(mem, row.Hidden.Text);
    Array_Push(&rows, row);
  }
  // Keep cache extents independently aligned when replacing the tail.
  ArenaAlloc(mem, 0, alignof(SpanLine));
  return rows;
}

void CacheEntry(LineModel* m, Entry* entry, bool showPreview) {
  ArenaReset(&m->LayoutMem);
  LineModel painter = {};
  painter.RowsMem = m->LayoutMem;
  painter.Width = m->Width;
  painter.Color = m->Color;
  painter.Rounded = m->Rounded;
  painter.Accent = m->Accent;
  painter.Lines = Array_Make<SpanLine>(&painter.RowsMem, 8);
  BuildEntryRows(&painter, entry, showPreview);
  m->LayoutMem = painter.RowsMem;

  m->LiveRowsBytes -= entry->RowsBytes;
  if (entry->RowsBytes && entry->RowsMark + entry->RowsBytes == m->RowsMem.Used)
    ArenaRestore(&m->RowsMem, entry->RowsMark);
  entry->RowsMark = m->RowsMem.Used;
  // Own the rendered bytes as well as the spans: markdown scratch and mutable
  // tool/editor buffers must never be referenced by a retained display row.
  entry->Rows = CopyRows(&m->RowsMem, painter.Lines);
  entry->RowsBytes = m->RowsMem.Used - entry->RowsMark;
  m->LiveRowsBytes += entry->RowsBytes;
  entry->PreviewShown = showPreview;
  entry->Dirty = false;
}

bool CompactRows(LineModel* m) {
  if (m->RowsMem.Used <= (usize)64 * 1024 ||
      m->RowsMem.Used <= m->LiveRowsBytes * 2)
    return false;
  Arena old = m->RowsMem;
  m->RowsMem = ArenaCreate(LINE_ROWS_RESERVE);
  m->LiveRowsBytes = 0;
  for (u32 i = 0; i < m->Entries.Count; i++) {
    Entry* entry = &m->Entries.Data[i];
    entry->RowsMark = m->RowsMem.Used;
    entry->Rows = CopyRows(&m->RowsMem, entry->Rows);
    entry->RowsBytes = m->RowsMem.Used - entry->RowsMark;
    m->LiveRowsBytes += entry->RowsBytes;
  }
  ArenaRelease(&old);
  return true;
}

void InvalidateEntry(LineModel* m, u32 index) {
  m->Entries.Data[index].Dirty = true;
  if (index < m->FirstDirtyEntry) m->FirstDirtyEntry = index;
}

void InvalidateAll(LineModel* m) {
  for (u32 i = 0; i < m->Entries.Count; i++) {
    m->Entries.Data[i].Dirty = true;
    m->Entries.Data[i].PreviewRows = 0xffffffffu;
  }
  m->FirstDirtyEntry = 0;
  m->PreviewBudgetDirty = true;
}

const SpanLine* FlatRow(const LineModel* m, u32 index) {
  return &m->Lines.Data[(m->LinesHead + index) % m->Lines.Capacity];
}

void EnsureFlatCapacity(LineModel* m, u32 count) {
  if (count <= m->Lines.Capacity) return;
  u32 capacity = m->Lines.Capacity;
  while (capacity < count) capacity *= 2;
  Array<SpanLine> lines = Array_Make<SpanLine>(m->Mem, capacity);
  for (u32 i = 0; i < m->Lines.Count; i++) Array_Push(&lines, *FlatRow(m, i));
  m->Lines = lines;
  m->LinesHead = 0;
}

u32 EntryAtRow(const LineModel* m, u64 row) {
  u32 first = 0;
  u32 end = m->Entries.Count;
  while (first < end) {
    u32 middle = first + (end - first) / 2;
    if (m->Entries.Data[middle].RowStart <= row) first = middle + 1;
    else end = middle;
  }
  return first ? first - 1 : 0;
}

u32 EntryIndexForId(const LineModel* m, u64 id) {
  u32 first = 0;
  u32 end = m->Entries.Count;
  while (first < end) {
    u32 middle = first + (end - first) / 2;
    if (m->Entries.Data[middle].Id < id) first = middle + 1;
    else end = middle;
  }
  return first;
}

void WriteFlatRange(LineModel* m, u64 start, u64 end, u32 firstEntry = NO_ENTRY) {
  if (start >= end) return;
  if (firstEntry == NO_ENTRY) firstEntry = EntryAtRow(m, start);
  for (u32 i = firstEntry; i < m->Entries.Count; i++) {
    const Entry* entry = &m->Entries.Data[i];
    if (entry->RowStart >= end) break;
    u64 contentStart = entry->RowStart + (entry->Separator ? 1u : 0u);
    if (entry->Separator && entry->RowStart >= start) {
      u32 row = (u32)(entry->RowStart - m->RowOffset);
      m->Lines.Data[(m->LinesHead + row) % m->Lines.Capacity] = {};
    }
    u64 first = contentStart > start ? contentStart : start;
    u64 last = contentStart + entry->Rows.Count;
    if (last > end) last = end;
    for (u64 row = first; row < last; row++) {
      u32 visible = (u32)(row - m->RowOffset);
      m->Lines.Data[(m->LinesHead + visible) % m->Lines.Capacity] =
          entry->Rows.Data[(u32)(row - contentStart)];
    }
  }
}

bool PreviewShown(const LineModel* m, const Entry* entry) {
  // A running call's tail is capped at LIVE_TAIL_ROWS, so only finished
  // previews draw on the expanded-row budget.
  return entry->Kind == EntryKind::Tool && !entry->Collapsed && entry->Tool.Preview.Length &&
         (!entry->Tool.Done || entry->Id >= m->ExpandedEntryId);
}

void CacheIfDirty(LineModel* m, Entry* entry) {
  bool preview = PreviewShown(m, entry);
  if (entry->Dirty || entry->PreviewShown != preview) CacheEntry(m, entry, preview);
}

void RemoveEntries(LineModel* m, u32 count) {
  for (u32 i = 0; i < count; i++) {
    const Entry* entry = &m->Entries.Data[i];
    m->LiveRowsBytes -= entry->RowsBytes;
    if (entry->Kind == EntryKind::Tool && entry->Tool.Live) m->LiveTools--;
  }
  m->Entries.Count -= count;
  memmove(m->Entries.Data, m->Entries.Data + count, m->Entries.Count * sizeof(Entry));
}

bool TrimNewEntries(LineModel* m) {
  if (!m->TrimPending || m->HasAssistant || m->FirstDirtyEntry != m->LayoutEntries)
    return false;
  u32 total = 0;
  const Entry* next = nullptr;
  // Size a new batch backwards, so a capped hydration never formats its
  // discarded prefix. Existing entries are not visited on an ordinary append.
  for (u32 i = m->Entries.Count; i > m->LayoutEntries; i--) {
    Entry* entry = &m->Entries.Data[i - 1];
    u32 removed = 0;
    if (total > m->Cap) {
      removed = i;
    } else {
      CacheIfDirty(m, entry);
      u32 cost = entry->Rows.Count;
      if (cost && next && NeedsSeparator(entry, next)) cost++;
      if (i < m->Entries.Count && cost > m->Cap - total) removed = i;
      else {
        total += cost;
        if (entry->Rows.Count) next = entry;
      }
    }
    if (removed) {
      RemoveEntries(m, removed);
      m->RowsBegin = m->RowsEnd;
      m->LayoutEntries = 0;
      m->LastVisibleEntry = NO_ENTRY;
      m->FirstDirtyEntry = 0;
      return true;
    }
  }
  return false;
}

bool TrimEntries(LineModel* m) {
  if (!m->TrimPending || m->HasAssistant || m->RowsEnd - m->RowsBegin <= m->Cap ||
      m->Entries.Count < 2)
    return false;
  u64 cutoff = m->RowsEnd - m->Cap;
  u32 removed = 0;
  for (u32 i = 0; i < m->Entries.Count; i++) {
    const Entry* entry = &m->Entries.Data[i];
    if (!entry->Rows.Count) continue;
    if (entry->RowStart + (entry->Separator ? 1u : 0u) >= cutoff) break;
    removed = i + 1;
  }
  if (removed == m->Entries.Count) removed--;
  if (!removed) return false;
  RemoveEntries(m, removed);
  m->RowsBegin = m->RowsEnd;
  m->LastVisibleEntry = NO_ENTRY;
  for (u32 i = 0; i < m->Entries.Count; i++) {
    Entry* entry = &m->Entries.Data[i];
    entry->PreviousVisible = m->LastVisibleEntry;
    if (!entry->Rows.Count) continue;
    if (m->LastVisibleEntry == NO_ENTRY) {
      if (entry->Separator) entry->RowStart++;
      entry->Separator = false;
      m->RowsBegin = entry->RowStart;
      for (u32 j = 0; j < i; j++) m->Entries.Data[j].RowStart = m->RowsBegin;
    }
    m->LastVisibleEntry = i;
  }
  m->LayoutEntries = m->Entries.Count;
  return true;
}

void RefreshPreviewBudget(LineModel* m) {
  if (!m->PreviewBudgetDirty) return;
  u64 old = m->ExpandedEntryId;
  m->ExpandedEntryId = ExpandedEntryId(m);
  m->PreviewBudgetDirty = false;
  if (old == m->ExpandedEntryId) return;
  u64 changed = old < m->ExpandedEntryId ? old : m->ExpandedEntryId;
  u32 first = EntryIndexForId(m, changed);
  if (first < m->FirstDirtyEntry) m->FirstDirtyEntry = first;
}

void EnsureLayout(const LineModel* m) {
  LineModel_Layout(const_cast<LineModel*>(m));
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
        Array_Push(&current.Spans, Span{token, span.St, false, span.Live});
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
                     Span{StrView{token.Data + segmentStart, j - segmentStart}, span.St, false,
                          span.Live});
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
                    bool color, bool rounded, u8 accent) {
  *m = {};
  m->Mem = mem;
  m->RowsMem = ArenaCreate(LINE_ROWS_RESERVE);
  m->LayoutMem = ArenaCreate(LINE_ROWS_RESERVE);
  m->Entries = Array_Make<Entry>(mem, 16);
  m->Lines = Array_Make<SpanLine>(mem, 128);
  m->Width = width ? width : 80;
  m->Cap = cap ? cap : 5000;
  m->AssistantStart = 0;
  m->AssistantRowOffset = 0;
  m->NextEntryId = 1;
  m->DisplayRevision = 1;
  m->FirstDirtyEntry = NO_ENTRY;
  m->LastVisibleEntry = NO_ENTRY;
  m->ExpandedEntryId = ~(u64)0;
  m->HasAssistant = false;
  m->Color = color;
  m->Rounded = rounded;
  m->Accent = accent;
}

void LineModel_Release(LineModel* m) {
  ArenaRelease(&m->RowsMem);
  ArenaRelease(&m->LayoutMem);
}

u32 LineModel_Count(const LineModel* m) {
  EnsureLayout(m);
  return m->Lines.Count;
}

bool LineModel_FitsAtWidth(LineModel* m, u16 width, u32 maxRows) {
  EnsureLayout(m);
  if (maxRows >= m->Cap) return true;
  if (width < 1) width = 1;
  if (width == m->Width) return m->Lines.Count <= maxRows;

  // A scrollbar probe need not format a long history at every frame.
  u32 visible = 0;
  for (u32 i = 0; i < m->Entries.Count; i++)
    if (EntryVisible(&m->Entries.Data[i]) && ++visible > maxRows) return false;

  LineModel probe = {};
  probe.RowsMem = ArenaCreate(LINE_ROWS_RESERVE);
  probe.LayoutMem = ArenaCreate(LINE_ROWS_RESERVE);
  probe.Width = width;
  probe.Cap = m->Cap;
  probe.Color = m->Color;
  probe.Rounded = m->Rounded;
  probe.Accent = m->Accent;
  probe.Entries = Array_Make<Entry>(&probe.RowsMem, m->Entries.Count);
  probe.Lines = Array_Make<SpanLine>(&probe.RowsMem, 32);
  for (u32 i = 0; i < m->Entries.Count; i++) {
    Entry entry = m->Entries.Data[i];
    entry.PreviewRows = 0xffffffffu;
    Array_Push(&probe.Entries, entry);
  }
  probe.ExpandedEntryId = ExpandedEntryId(&probe);
  const Entry* previous = nullptr;
  for (u32 i = 0; i < probe.Entries.Count; i++) {
    const Entry* entry = &probe.Entries.Data[i];
    if (!EntryVisible(entry)) continue;
    if (NeedsSeparator(previous, entry)) AddBlank(&probe);
    BuildEntryRows(&probe, entry, PreviewShown(&probe, entry));
    previous = entry;
    if (probe.Lines.Count > maxRows) break;
  }
  bool fits = probe.Lines.Count <= maxRows;
  ArenaRelease(&probe.LayoutMem);
  ArenaRelease(&probe.RowsMem);
  return fits;
}

const SpanLine* LineModel_At(const LineModel* m, u32 i) {
  EnsureLayout(m);
  return i < m->Lines.Count ? FlatRow(m, i) : nullptr;
}

u64 LineModel_RowEntryId(const LineModel* m, u32 row) {
  const SpanLine* line = LineModel_At(m, row);
  return line ? line->EntryId : 0;
}

u64 LineModel_DisplayRevision(const LineModel* m) {
  EnsureLayout(m);
  return m->DisplayRevision;
}

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
    StrView source = span.Live && layout->LiveGlyph.Length ? layout->LiveGlyph : span.Text;
    u16 width = 0;
    StrView text =
        PrefixWithinCells(source, (u16)(layout->LeftLimit - iterator->Column), &width);
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
              : StyleMake(MUTED_FG, COL_DEFAULT, ATTR_DIM);
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
    const SpanLine* line = FlatRow(m, m->AssistantStart + i);
    DisplayRowLayout layout = LineModel_LayoutDisplayRow(line, m->Width);
    result.FixedRows = HashDisplayValue(result.FixedRows, layout.ContentEnd);
    result.FixedRows =
        HashDisplayValue(result.FixedRows,
                         DisplayRowPrefixHash(line, m->Width, layout.ContentEnd));
  }
  const SpanLine* last = FlatRow(m, m->Lines.Count - 1u);
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
    const SpanLine* line = FlatRow(m, m->AssistantStart + i);
    DisplayRowLayout layout = LineModel_LayoutDisplayRow(line, m->Width);
    fixedRows = HashDisplayValue(fixedRows, layout.ContentEnd);
    fixedRows =
        HashDisplayValue(fixedRows,
                         DisplayRowPrefixHash(line, m->Width, layout.ContentEnd));
  }
  if (fixedRows != previous.FixedRows) return false;
  const SpanLine* last = FlatRow(m, m->AssistantStart + previous.RowCount - 1u);
  DisplayRowLayout layout = LineModel_LayoutDisplayRow(last, m->Width);
  return layout.ContentEnd >= previous.LastEnd &&
         DisplayRowPrefixHash(last, m->Width, previous.LastEnd) == previous.LastRow;
}

}  // namespace

void LineModel_Layout(LineModel* m) {
  if (m->FirstDirtyEntry == NO_ENTRY && !m->TrimPending && !m->PreviewBudgetDirty) return;
  u32 previousStart = m->AssistantStart;
  u32 previousOffset = m->AssistantRowOffset;
  bool streaming = m->HasAssistant;
  bool trackAssistant = m->Entries.Count &&
                        m->Entries.Data[m->Entries.Count - 1].Kind == EntryKind::Assistant;
  bool assistantChanged = trackAssistant &&
                          m->Entries.Data[m->Entries.Count - 1].Dirty;
  AssistantPrefixFingerprint previous = {};
  if (assistantChanged) previous = CaptureAssistantPrefix(m);

  u64 oldOffset = m->RowOffset;
  u64 oldEnd = oldOffset + m->Lines.Count;
  RefreshPreviewBudget(m);
  bool removed = TrimNewEntries(m);
  u32 first = m->FirstDirtyEntry;
  u64 suffixStart = first < m->LayoutEntries ? m->Entries.Data[first].RowStart : m->RowsEnd;
  u64 row = suffixStart;
  u32 prior = first < m->LayoutEntries ? m->Entries.Data[first].PreviousVisible
                                      : m->LastVisibleEntry;
  for (u32 i = first; i < m->Entries.Count; i++) {
    Entry* entry = &m->Entries.Data[i];
    CacheIfDirty(m, entry);
    entry->PreviousVisible = prior;
    entry->RowStart = row;
    entry->Separator = entry->Rows.Count && prior != NO_ENTRY &&
                       NeedsSeparator(&m->Entries.Data[prior], entry);
    row += entry->Rows.Count + (entry->Separator ? 1u : 0u);
    if (entry->Rows.Count) prior = i;
  }
  m->RowsEnd = row;
  m->LastVisibleEntry = prior;
  m->LayoutEntries = m->Entries.Count;
  if (TrimEntries(m)) removed = true;
  bool relocated = CompactRows(m);

  u64 offset = m->RowsEnd - m->RowsBegin > m->Cap ? m->RowsEnd - m->Cap : m->RowsBegin;
  u32 count = (u32)(m->RowsEnd - offset);
  EnsureFlatCapacity(m, count);
  u32 capacity = m->Lines.Capacity;
  if (offset >= oldOffset)
    m->LinesHead = (m->LinesHead + (u32)((offset - oldOffset) % capacity)) % capacity;
  else
    m->LinesHead = (m->LinesHead + capacity - (u32)((oldOffset - offset) % capacity)) % capacity;
  m->RowOffset = offset;
  m->Lines.Count = count;
  // Logical coordinates keep the surviving prefix in the same ring slots.
  // Only an uncovered prefix or the changed suffix needs new metadata.
  u64 stableStart = offset > oldOffset ? offset : oldOffset;
  u64 stableEnd = suffixStart < oldEnd ? suffixStart : oldEnd;
  u32 changedEntry = !removed && first < m->Entries.Count ? first : NO_ENTRY;
  if (relocated || stableStart >= stableEnd) {
    WriteFlatRange(m, offset, m->RowsEnd,
                   !relocated && offset >= suffixStart ? changedEntry : NO_ENTRY);
  } else {
    WriteFlatRange(m, offset, stableStart);
    WriteFlatRange(m, stableEnd, m->RowsEnd, changedEntry);
  }
  if (trackAssistant) {
    u64 start = m->Entries.Data[m->Entries.Count - 1].RowStart;
    m->AssistantStart = start > offset ? (u32)(start - offset) : 0;
    m->AssistantRowOffset = offset > start ? (u32)(offset - start) : 0;
  }
  if (removed || (assistantChanged &&
      (m->AssistantStart != previousStart || m->AssistantRowOffset != previousOffset ||
       !AssistantPrefixIsStable(m, previous))))
    AdvanceDisplayRevision(m);
  if (!streaming) m->AssistantRowOffset = 0;
  m->FirstDirtyEntry = NO_ENTRY;
  m->TrimPending = false;
  ArenaReset(&m->LayoutMem);
  if (!streaming) {
    ArenaTrim(&m->LayoutMem, (usize)64 * 1024);
    ArenaTrim(&m->RowsMem, m->RowsMem.Used + (usize)64 * 1024);
  }
}

bool LineModel_FindEntryRows(const LineModel* m, u64 entryId, u32* firstRow, u32* rowCount) {
  EnsureLayout(m);
  if (entryId == 0) return false;
  u32 index = EntryIndexForId(m, entryId);
  if (index == m->Entries.Count || m->Entries.Data[index].Id != entryId) return false;
  const Entry* entry = &m->Entries.Data[index];
  u64 start = entry->RowStart + (entry->Separator ? 1u : 0u);
  u64 end = start + entry->Rows.Count;
  if (start < m->RowOffset) start = m->RowOffset;
  if (end > m->RowsEnd) end = m->RowsEnd;
  if (start >= end) return false;
  if (firstRow) *firstRow = (u32)(start - m->RowOffset);
  if (rowCount) *rowCount = (u32)(end - start);
  return true;
}

bool LineModel_IsEntryCollapsed(const LineModel* m, u64 entryId) {
  u32 index = EntryIndexForId(m, entryId);
  return index < m->Entries.Count && m->Entries.Data[index].Id == entryId &&
         m->Entries.Data[index].Collapsed;
}

void LineModel_SetWidth(LineModel* m, u16 width) {
  if (width < 1) width = 1;
  if (width == m->Width) return;
  m->Width = width;
  InvalidateAll(m);
  m->TrimPending = true;
  AdvanceDisplayRevision(m);
}

bool LineModel_SetRounded(LineModel* m, bool rounded) {
  if (m->Rounded == rounded) return false;
  m->Rounded = rounded;
  InvalidateAll(m);
  AdvanceDisplayRevision(m);
  return true;
}

bool LineModel_ToggleEntry(LineModel* m, u64 entryId) {
  u32 index = EntryIndexForId(m, entryId);
  if (index == m->Entries.Count || m->Entries.Data[index].Id != entryId) return false;
  Entry* entry = &m->Entries.Data[index];
  entry->Collapsed = !entry->Collapsed;
  InvalidateEntry(m, index);
  if (entry->Kind == EntryKind::Tool) m->PreviewBudgetDirty = true;
  AdvanceDisplayRevision(m);
  return true;
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
    InvalidateEntry(m, i);
    changed = true;
  }
  if (changed) {
    m->PreviewBudgetDirty = true;
    AdvanceDisplayRevision(m);
  }
  return changed;
}

void LineModel_BeginAssistant(LineModel* m) {
  FFI(m->HasAssistant);
  LineModel_EndThinking(m);
  Entry entry = {};
  entry.Id = m->NextEntryId++;
  FFI(entry.Id == 0);
  entry.Kind = EntryKind::Assistant;
  entry.Text.Content = StrBuf_Make(m->Mem, 64);
  Array_Push(&m->Entries, entry);
  m->HasAssistant = true;
  m->AssistantStart = m->Lines.Count;
  m->AssistantRowOffset = 0;
  InvalidateEntry(m, m->Entries.Count - 1);
}

void LineModel_AppendAssistant(LineModel* m, StrView delta) {
  FFI(!m->HasAssistant || m->Entries.Count == 0);
  if (delta.Length == 0) return;
  Entry* entry = &m->Entries.Data[m->Entries.Count - 1];
  StrBuf_Append(&entry->Text.Content, delta);
  InvalidateEntry(m, m->Entries.Count - 1);
}

void LineModel_EndAssistant(LineModel* m) {
  FFI(!m->HasAssistant);
  m->HasAssistant = false;
  m->TrimPending = true;
}

void LineModel_AddNotice(LineModel* m, EntryKind kind, StrView text) {
  FFI(kind != EntryKind::Info && kind != EntryKind::Error);
  FFI(m->HasAssistant);
  LineModel_EndThinking(m);
  Entry entry = {};
  entry.Id = m->NextEntryId++;
  FFI(entry.Id == 0);
  entry.Kind = kind;
  entry.Text.Content = StrBuf_Make(m->Mem, text.Length + 1);
  StrBuf_Append(&entry.Text.Content, text);
  Array_Push(&m->Entries, entry);
  InvalidateEntry(m, m->Entries.Count - 1);
  m->TrimPending = true;
  AdvanceDisplayRevision(m);
}

void LineModel_AddUser(LineModel* m, StrView text, u64 timestampMs, bool steered) {
  FFI(m->HasAssistant);
  LineModel_EndThinking(m);
  Entry entry = {};
  entry.Id = m->NextEntryId++;
  FFI(entry.Id == 0);
  entry.Kind = EntryKind::User;
  entry.Text.Content = StrBuf_Make(m->Mem, text.Length + 1);
  StrBuf_Append(&entry.Text.Content, text);
  StrView time = FormatTimestamp(m->Mem, timestampMs);
  if (steered) {
    StrView mark = SVL("\xe2\x86\xb3 steer");
    StrBuf label = StrBuf_Make(m->Mem, mark.Length + time.Length + 4);
    StrBuf_Append(&label, mark);
    if (time.Length) {
      StrBuf_AppendCStr(&label, " \xc2\xb7 ");
      StrBuf_Append(&label, time);
    }
    time = StrBuf_View(&label);
  }
  entry.Text.Timestamp = time;
  Array_Push(&m->Entries, entry);
  InvalidateEntry(m, m->Entries.Count - 1);
  m->TrimPending = true;
  AdvanceDisplayRevision(m);
}

void LineModel_AddFooter(LineModel* m, TurnStats stats) {
  FFI(m->HasAssistant);
  LineModel_EndThinking(m);
  Entry entry = {};
  entry.Id = m->NextEntryId++;
  FFI(entry.Id == 0);
  entry.Kind = EntryKind::Footer;
  entry.Footer = stats;
  Array_Push(&m->Entries, entry);
  InvalidateEntry(m, m->Entries.Count - 1);
  m->TrimPending = true;
  AdvanceDisplayRevision(m);
}

bool LineModel_SetThinking(LineModel* m, u64 startMs, u64 nowMs) {
  FFI(m->HasAssistant);
  u32 seconds = nowMs > startMs ? (u32)((nowMs - startMs) / 1000) : 0;
  if (m->Entries.Count) {
    u32 last = m->Entries.Count - 1;
    Entry* tail = &m->Entries.Data[last];
    if (tail->Kind == EntryKind::Thinking) {
      if (tail->Thinking.ElapsedSeconds == seconds) return false;
      tail->Thinking.ElapsedSeconds = seconds;
      InvalidateEntry(m, last);
      return true;
    }
  }
  Entry entry = {};
  entry.Id = m->NextEntryId++;
  FFI(entry.Id == 0);
  entry.Kind = EntryKind::Thinking;
  entry.Thinking.ElapsedSeconds = seconds;
  Array_Push(&m->Entries, entry);
  InvalidateEntry(m, m->Entries.Count - 1);
  AdvanceDisplayRevision(m);
  return true;
}

// The thinking row is only ever last, so its rows end the layout and rewinding
// RowsEnd to where it began is all the relayout needs.
bool LineModel_EndThinking(LineModel* m) {
  if (!m->Entries.Count) return false;
  u32 index = m->Entries.Count - 1;
  const Entry* entry = &m->Entries.Data[index];
  if (entry->Kind != EntryKind::Thinking) return false;
  m->LiveRowsBytes -= entry->RowsBytes;
  if (entry->RowsBytes && entry->RowsMark + entry->RowsBytes == m->RowsMem.Used)
    ArenaRestore(&m->RowsMem, entry->RowsMark);
  if (index < m->LayoutEntries) {
    m->RowsEnd = entry->RowStart;
    m->LastVisibleEntry = entry->PreviousVisible;
    m->LayoutEntries = index;
  }
  m->Entries.Count = index;
  if (m->FirstDirtyEntry > index) m->FirstDirtyEntry = index;
  AdvanceDisplayRevision(m);
  return true;
}

void LineModel_AddTool(LineModel* m, StrView toolId, StrView name, StrView argsJson,
                       u64 startMs) {
  FFI(m->HasAssistant);
  LineModel_EndThinking(m);
  Entry entry = {};
  entry.Id = m->NextEntryId++;
  FFI(entry.Id == 0);
  entry.Kind = EntryKind::Tool;
  entry.PreviewRows = 0xffffffffu;
  entry.Collapsed = true;
  entry.Tool.Id = StrView_Dup(m->Mem, toolId);
  entry.Tool.Preview = StrBuf_Make(m->Mem, 256);
  entry.Tool.StartMs = startMs;
  entry.Tool.Live = startMs != 0;
  if (entry.Tool.Live) m->LiveTools++;
  ToolView_Init(m->Mem, &entry.Tool.View, name, argsJson);
  Array_Push(&m->Entries, entry);
  InvalidateEntry(m, m->Entries.Count - 1);
  m->TrimPending = true;
  AdvanceDisplayRevision(m);
}

bool LineModel_UpdateTool(LineModel* m, StrView toolId, StrView preview) {
  for (u32 i = m->Entries.Count; i > 0; i--) {
    Entry* entry = &m->Entries.Data[i - 1];
    if (entry->Kind != EntryKind::Tool || entry->Tool.Done) continue;
    if (!StrView_Eq(entry->Tool.Id, toolId)) continue;
    bool hadPreview = entry->Tool.Preview.Length != 0;
    StrBuf_Clear(&entry->Tool.Preview);
    StrBuf_Append(&entry->Tool.Preview, preview);
    // A collapsed row changes only when its hidden-content mark appears.
    if (!entry->Collapsed || hadPreview != (preview.Length != 0)) InvalidateEntry(m, i - 1);
    return true;
  }
  return false;
}

bool LineModel_FinishTool(LineModel* m, StrView toolId, StrView result, StrView preview,
                          bool isError, u64 endMs) {
  for (u32 i = m->Entries.Count; i > 0; i--) {
    Entry* entry = &m->Entries.Data[i - 1];
    if (entry->Kind != EntryKind::Tool) continue;
    if (!StrView_Eq(entry->Tool.Id, toolId)) continue;
    if (entry->Tool.Done) return true;
    entry->Tool.Done = true;
    entry->Tool.Error = isError;
    entry->Tool.EndMs = endMs;
    if (entry->Tool.Live) {
      entry->Tool.Live = false;
      m->LiveTools--;
    }
    InvalidateEntry(m, i - 1);
    entry->PreviewRows = 0xffffffffu;
    StrBuf_Clear(&entry->Tool.Preview);
    StrBuf_Append(&entry->Tool.Preview, preview);
    ToolView_Finish(&entry->Tool.View, result, isError);
    if (!entry->Collapsed && preview.Length) m->PreviewBudgetDirty = true;
    AdvanceDisplayRevision(m);
    return true;
  }
  return false;
}

bool LineModel_TickTools(LineModel* m, u64 nowMs) {
  bool changed = false;
  u32 remaining = m->LiveTools;
  for (u32 i = m->Entries.Count; i > 0 && remaining; i--) {
    Entry* entry = &m->Entries.Data[i - 1];
    if (entry->Kind != EntryKind::Tool || !entry->Tool.Live) continue;
    remaining--;
    u64 elapsed = nowMs > entry->Tool.StartMs ? nowMs - entry->Tool.StartMs : 0;
    u32 seconds = (u32)(elapsed / 1000);
    if (seconds == entry->Tool.ElapsedSeconds) continue;
    entry->Tool.ElapsedSeconds = seconds;
    InvalidateEntry(m, i - 1);
    changed = true;
  }
  return changed;
}

void LineModel_SettleTools(LineModel* m) {
  for (u32 i = m->Entries.Count; i > 0 && m->LiveTools; i--) {
    Entry* entry = &m->Entries.Data[i - 1];
    if (entry->Kind != EntryKind::Tool || !entry->Tool.Live) continue;
    entry->Tool.Live = false;
    m->LiveTools--;
    InvalidateEntry(m, i - 1);
  }
}
