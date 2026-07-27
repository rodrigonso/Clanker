#include "term/linemodel.h"

#include <string.h>
#include <time.h>

#include "term/markdown.h"

namespace {

const usize LINE_ROWS_RESERVE = (usize)64 * 1024 * 1024;
const u32 ICON_FIELD_CELLS = 2;

// Bullet color scheme: a bright accent for user messages, a subtler (dim)
// shade of the same accent for tool calls, and near-white for assistant
// responses. Red is reserved strictly for error state. All bullets share the
// same square glyph.
const u8 ACCENT_FG = 214;
const u8 ASSISTANT_FG = 255;
StrView SquareMarker() { return SVL("\xe2\x96\xa0"); }

bool g_lmColor = true;

StrView BufView(const StrBuf& buf) { return StrView{buf.Data, buf.Length}; }

Style LStyle(u8 fg, u8 attr) {
  return Style{g_lmColor ? fg : COL_DEFAULT, COL_DEFAULT, attr};
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
  u32 width = TextWidth(marker);
  return width < ICON_FIELD_CELLS ? ICON_FIELD_CELLS : width;
}

StrView IconIndent(Arena* arena, StrView marker) {
  return Spaces(arena, IconFieldWidth(marker) + 1u);
}

void PushIcon(Arena* arena, Array<Span>* spans, StrView marker, Style style) {
  Array_Push(spans, Span{marker, style});
  u32 markerWidth = TextWidth(marker);
  u32 gap = (markerWidth < ICON_FIELD_CELLS ? ICON_FIELD_CELLS : markerWidth) - markerWidth + 1u;
  Array_Push(spans, Span{Spaces(arena, gap), style, true});
}

void InitRow(Arena* a, SpanLine* row, u32 spanCap = 4) {
  *row = {};
  row->Spans = Array_Make<Span>(a, spanCap);
}

void StartWrappedRow(Arena* a, const SpanLine* in, SpanLine* row, bool first, bool showRight,
                     u16 width, u16* col, u16* limit) {
  InitRow(a, row);
  if (first && showRight) {
    row->Right = in->Right;
    row->HasRight = true;
  }

  *limit = width;
  if (first && showRight) {
    u32 rightWidth = TextWidth(in->Right.Text);
    *limit = (u16)(width - rightWidth - 1u);
  }

  *col = 0;
  if (!first && in->HasContinuation) {
    u32 continuationWidth = TextWidth(in->Continuation.Text);
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
  time_t seconds = (time_t)(timestampMs / 1000ull);
  struct tm local = {};
#if defined(_WIN32)
  if (localtime_s(&local, &seconds) != 0) return StrView{};
#else
  if (!localtime_r(&seconds, &local)) return StrView{};
#endif
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
  StrView marker = SquareMarker();
  StrView indent = IconIndent(&m->RowsMem, marker);
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
      PushIcon(&m->RowsMem, &logical.Spans, marker, LStyle(ACCENT_FG, ATTR_BOLD));
    } else {
      Array_Push(&logical.Spans, Span{indent, StyleDefault(), true});
    }
    if (line.Length) Array_Push(&logical.Spans, Span{line, StyleDefault()});
    logical.Continuation = Span{indent, StyleDefault(), true};
    logical.HasContinuation = true;
    if (firstLine && entry->Text.Timestamp.Length) {
      logical.Right = Span{entry->Text.Timestamp, LStyle(244, ATTR_NONE)};
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
  StrView marker = SquareMarker();
  StrView indent = IconIndent(&m->RowsMem, marker);

  Array<SpanLine> logical = Array_Make<SpanLine>(&m->RowsMem, 8);
  Markdown_Render(&m->RowsMem, text, &logical);
  for (u32 i = 0; i < logical.Count; i++) {
    SpanLine prefixed;
    InitRow(&m->RowsMem, &prefixed, logical.Data[i].Spans.Count + 2);
    if (i == 0) {
      PushIcon(&m->RowsMem, &prefixed.Spans, marker, LStyle(ASSISTANT_FG, ATTR_BOLD));
    } else {
      Array_Push(&prefixed.Spans, Span{indent, StyleDefault(), true});
    }
    for (u32 j = 0; j < logical.Data[i].Spans.Count; j++)
      Array_Push(&prefixed.Spans, logical.Data[i].Spans.Data[j]);
    prefixed.Continuation = Span{indent, StyleDefault(), true};
    prefixed.HasContinuation = true;
    WrapAndAppend(m, &prefixed);
  }
}

void BuildTool(LineModel* m, const Entry* entry, bool expandPreview) {
  StrView marker = SquareMarker();
  Style markerSt = entry->Tool.Error ? LStyle(196, ATTR_BOLD) : LStyle(ACCENT_FG, ATTR_DIM);
  Style label = entry->Tool.Error ? LStyle(196, ATTR_BOLD) : LStyle(252, ATTR_BOLD);
  Style detail = entry->Tool.Error ? LStyle(196, ATTR_NONE) : LStyle(252, ATTR_NONE);
  StrView indent = IconIndent(&m->RowsMem, marker);

  SpanLine logical;
  InitRow(&m->RowsMem, &logical, 7);
  PushIcon(&m->RowsMem, &logical.Spans, marker, markerSt);
  if (entry->Tool.View.Label.Length) {
    Array_Push(&logical.Spans, Span{entry->Tool.View.Label, label});
    if (entry->Tool.View.Detail.Length) Array_Push(&logical.Spans, Span{SVL(" "), detail});
  }
  if (entry->Tool.View.Detail.Length)
    Array_Push(&logical.Spans, Span{BufView(entry->Tool.View.Detail), detail});
  if (entry->Tool.View.Outcome.Length) {
    Array_Push(&logical.Spans, Span{SVL("  "), detail});
    Array_Push(&logical.Spans,
               Span{BufView(entry->Tool.View.Outcome),
                    entry->Tool.Error ? LStyle(196, ATTR_NONE) : LStyle(244, ATTR_NONE)});
  }
  logical.Continuation = Span{indent, StyleDefault(), true};
  logical.HasContinuation = true;
  WrapAndAppend(m, &logical);

  if (expandPreview && entry->Tool.Done && entry->Tool.Preview.Length) {
    AppendPlain(m, BufView(entry->Tool.Preview),
                entry->Tool.Error ? LStyle(196, ATTR_NONE) : LStyle(244, ATTR_DIM), indent, indent);
  }
}

void BuildPlainTurn(LineModel* m, const Entry* entry, StrView marker, Style markerStyle,
                    Style textStyle) {
  StrView text = BufView(entry->Text.Content);
  StrView indent = IconIndent(&m->RowsMem, marker);
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
      PushIcon(&m->RowsMem, &logical.Spans, marker, markerStyle);
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
      BuildPlainTurn(m, entry, SVL("\xe2\x97\x8f"), LStyle(ACCENT_FG, ATTR_BOLD),
                     LStyle(244, ATTR_NONE));
      break;
    case EntryKind::Error:
      BuildPlainTurn(m, entry, SVL("!"), LStyle(196, ATTR_BOLD), LStyle(196, ATTR_BOLD));
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
    if (hidden) m->Lines.Data[rowStart].HasHiddenContent = true;
  }
}

void CropDisplayToCap(LineModel* m) {
  if (m->Lines.Count <= m->Cap) return;
  u32 drop = m->Lines.Count - m->Cap;
  memmove(m->Lines.Data, m->Lines.Data + drop, m->Cap * sizeof(SpanLine));
  m->Lines.Count = m->Cap;
  if (m->HasAssistant)
    m->AssistantStart = m->AssistantStart > drop ? m->AssistantStart - drop : 0;
}

u32 PreviewRowCount(LineModel* m, const Entry* entry) {
  usize mark = m->RowsMem.Used;
  Array<SpanLine> rows = Array_Make<SpanLine>(&m->RowsMem, 8);
  StrView indent = IconIndent(&m->RowsMem, SquareMarker());
  AppendPlainRows(&m->RowsMem, &rows, m->Width, BufView(entry->Tool.Preview),
                  LStyle(244, ATTR_DIM), indent, indent);
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

void TrimToCap(LineModel* m) {
  RebuildAll(m, false);
  while (m->Lines.Count > m->Cap && m->Entries.Count > 1) {
    memmove(m->Entries.Data, m->Entries.Data + 1, (m->Entries.Count - 1) * sizeof(Entry));
    m->Entries.Count--;
    RebuildAll(m, false);
  }
  CropDisplayToCap(m);
}

}  // namespace

void WrapSpanLine(Arena* a, const SpanLine* in, u16 width, Array<SpanLine>* out) {
  if (width < 1) width = 1;
  u32 rightWidth = in->HasRight ? TextWidth(in->Right.Text) : 0;
  bool showRight = in->HasRight && rightWidth + 3u < width;

  SpanLine current;
  u16 col = 0;
  u16 limit = width;
  bool first = true;
  StartWrappedRow(a, in, &current, first, showRight, width, &col, &limit);

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
          StartWrappedRow(a, in, &current, first, showRight, width, &col, &limit);
          continue;
        }
        u32 room = (u32)limit - col;
        u32 take = token.Length < room ? token.Length : room;
        Array_Push(&current.Spans, Span{StrView{token.Data, take}, span.St});
        col = (u16)(col + take);
        continue;
      }

      u32 wordWidth = TextWidth(token);
      if (col > 0 && (u32)col + wordWidth > limit) {
        Array_Push(out, current);
        first = false;
        StartWrappedRow(a, in, &current, first, showRight, width, &col, &limit);
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
            u32 charWidth = CharWidth(cp);
            if (charWidth < 1) charWidth = 1;
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
            StartWrappedRow(a, in, &current, first, showRight, width, &col, &limit);
          }
        }
      }
    }
  }
  Array_Push(out, current);
}

void LineModel_SetColorEnabled(bool on) {
  g_lmColor = on;
  Markdown_SetColorEnabled(on);
}

void LineModel_Init(LineModel* m, Arena* mem, u16 width, u32 cap) {
  m->Mem = mem;
  m->RowsMem = ArenaCreate(LINE_ROWS_RESERVE);
  m->Entries = Array_Make<Entry>(mem, 16);
  m->Lines = Array_Make<SpanLine>(mem, 128);
  m->Width = width ? width : 80;
  m->Cap = cap ? cap : 5000;
  m->AssistantStart = 0;
  m->AssistantRowsMark = 0;
  m->NextEntryId = 1;
  m->HasAssistant = false;
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
}

bool LineModel_ToggleEntry(LineModel* m, u64 entryId) {
  for (u32 i = 0; i < m->Entries.Count; i++) {
    Entry* entry = &m->Entries.Data[i];
    if (entry->Id != entryId) continue;
    entry->Collapsed = !entry->Collapsed;
    RebuildAll(m, true);
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
  if (changed) RebuildAll(m, true);
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
  RebuildAll(m, true);
}

void LineModel_AppendAssistant(LineModel* m, StrView delta) {
  FFI(!m->HasAssistant || m->Entries.Count == 0);
  Entry* entry = &m->Entries.Data[m->Entries.Count - 1];
  StrBuf_Append(&entry->Text.Content, delta);
  ArenaRestore(&m->RowsMem, m->AssistantRowsMark);
  m->Lines.Count = m->AssistantStart;
  BuildEntryRows(m, m->Entries.Count - 1, m->Entries.Count);
  CropDisplayToCap(m);
}

void LineModel_EndAssistant(LineModel* m) {
  FFI(!m->HasAssistant);
  m->HasAssistant = false;
  TrimToCap(m);
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
}

bool LineModel_FinishTool(LineModel* m, StrView toolId, StrView result, StrView preview, bool isError) {
  for (u32 i = m->Entries.Count; i > 0; i--) {
    Entry* entry = &m->Entries.Data[i - 1];
    if (entry->Kind != EntryKind::Tool || entry->Tool.Done) continue;
    if (!StrView_Eq(entry->Tool.Id, toolId)) continue;
    entry->Tool.Done = true;
    entry->Tool.Error = isError;
    StrBuf_Clear(&entry->Tool.Preview);
    StrBuf_Append(&entry->Tool.Preview, preview);
    ToolView_Finish(&entry->Tool.View, result, isError);
    RebuildAll(m, true);
    return true;
  }
  return false;
}
