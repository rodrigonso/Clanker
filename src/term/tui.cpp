#include "term/tui.h"

#include <string.h>

#include "agent/agent.h"
#include "base/sysinfo.h"
#include "term/cell.h"
#include "term/sanitize.h"

namespace {

const char* kSpinner[10] = {"\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8",
                            "\xe2\xa0\xbc", "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7",
                            "\xe2\xa0\x87", "\xe2\xa0\x8f"};
constexpr u64 kDoubleEscapeMs = 500;

bool g_color = true;
Style St(u8 fg, u8 attr) { return Style{g_color ? fg : COL_DEFAULT, COL_DEFAULT, attr}; }

// Append a C-string to a bounded buffer.
void Put(char* buf, u32 cap, u32* len, const char* s) {
  while (*s && *len < cap - 1) buf[(*len)++] = *s++;
}
void PutNum(char* buf, u32 cap, u32* len, u32 v) {
  char tmp[16];
  u32 n = 0;
  if (v == 0) tmp[n++] = '0';
  while (v) {
    tmp[n++] = (char)('0' + v % 10);
    v /= 10;
  }
  while (n && *len < cap - 1) buf[(*len)++] = tmp[--n];
}

// Format a count using best-fit byte-style unit scaling (B/KiB/MiB/GiB,
// 1024-based). Sub-unit values print one decimal place (e.g. "3.4KiB").
void PutScaled(char* buf, u32 cap, u32* len, u32 v) {
  const char* units[4] = {"B", "KiB", "MiB", "GiB"};
  u32 unit = 0;
  u64 whole = v;
  u64 scaled10 = whole * 10;  // one decimal digit while unit == 0 (bytes)
  while (whole >= 1024 && unit < 3) {
    scaled10 = (scaled10 + 512) / 1024;  // carry a rounded decimal digit through the shift
    whole /= 1024;
    unit++;
  }
  if (unit == 0) {
    PutNum(buf, cap, len, (u32)whole);
  } else {
    u32 intPart = (u32)(scaled10 / 10);
    u32 frac = (u32)(scaled10 % 10);
    PutNum(buf, cap, len, intPart);
    Put(buf, cap, len, ".");
    char d[2] = {(char)('0' + frac), 0};
    Put(buf, cap, len, d);
  }
  Put(buf, cap, len, units[unit]);
}

// ---- queue ----
bool QPush(Tui* t, TuiSubmission submission) {
  if (t->QCount >= 64) return false;
  t->Queue[t->QTail] = submission;
  t->QTail = (t->QTail + 1) % 64;
  t->QCount++;
  return true;
}
bool QPop(Tui* t, TuiSubmission* out) {
  if (t->QCount == 0) return false;
  *out = t->Queue[t->QHead];
  t->QHead = (t->QHead + 1) % 64;
  t->QCount--;
  return true;
}

bool IsInputSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

bool IsSlashCommand(StrView line) {
  u32 i = 0;
  while (i < line.Length && IsInputSpace(line.Data[i])) i++;
  return i < line.Length && line.Data[i] == '/';
}

bool HasPrefix(StrView text, StrView prefix) {
  return prefix.Length <= text.Length &&
         (prefix.Length == 0 || memcmp(text.Data, prefix.Data, prefix.Length) == 0);
}

bool CommandMatches(const TuiCommandSpec& command, StrView query) {
  return HasPrefix(command.Name, query) ||
         (command.Alias.Length && HasPrefix(command.Alias, query));
}

u32 CommandMatchCount(const Tui* t, StrView query) {
  u32 count = 0;
  for (u32 i = 0; i < t->CommandCount; i++)
    if (CommandMatches(t->Commands[i], query)) count++;
  return count;
}

const TuiCommandSpec* CommandMatchAt(const Tui* t, StrView query, u32 matchIndex) {
  for (u32 i = 0; i < t->CommandCount; i++) {
    if (!CommandMatches(t->Commands[i], query)) continue;
    if (matchIndex == 0) return &t->Commands[i];
    matchIndex--;
  }
  return nullptr;
}

bool CommandPaletteQuery(const Tui* t, StrView* out) {
  if (!t->Commands || t->CommandCount == 0 || t->CommandPaletteDismissed ||
      t->ModalResult == TuiModalResult::Pending)
    return false;

  StrView text = LineEdit_Text(&t->Edit);
  if (LineEdit_Cursor(&t->Edit) != text.Length) return false;
  u32 first = 0;
  while (first < text.Length && IsInputSpace(text.Data[first])) first++;
  if (first >= text.Length || text.Data[first] != '/') return false;
  for (u32 i = first + 1; i < text.Length; i++)
    if (IsInputSpace(text.Data[i])) return false;
  *out = StrView{text.Data + first + 1, text.Length - first - 1};
  return true;
}

void ResetCommandPalette(Tui* t) {
  t->CommandPaletteSelected = 0;
  t->CommandPaletteFirst = 0;
  t->CommandPaletteDismissed = false;
}

void Wake(Tui* t) {
  t->Dirty = true;
  Cond_Signal(&t->RenderCv);
}

void WakeInput(Tui* t) {
  t->InputDirty = true;
  Wake(t);
}

bool HasQueuedDisplayEvents(const Tui* t) {
  return t->DisplayHead < t->DisplayEvents.Count;
}

void CompactDisplayEvents(Tui* t) {
  if (t->DisplayHead == 0) return;
  if (t->DisplayHead == t->DisplayEvents.Count) {
    t->DisplayHead = 0;
    t->DisplayEvents.Count = 0;
    return;
  }
  if (t->DisplayHead < 64) return;
  u32 remaining = t->DisplayEvents.Count - t->DisplayHead;
  memmove(t->DisplayEvents.Data, t->DisplayEvents.Data + t->DisplayHead,
          sizeof(TuiDisplayEvent) * remaining);
  t->DisplayEvents.Count = remaining;
  t->DisplayHead = 0;
}

TuiDisplayEvent* QueueDisplayEvent(Tui* t, TuiDisplayEventKind kind) {
  CompactDisplayEvents(t);
  TuiDisplayEvent ev = {};
  ev.Kind = kind;
  return Array_Push(&t->DisplayEvents, ev);
}

void SetDisplayPayload(Tui* t, TuiDisplayEvent* ev, StrView text) {
  ev->Payload = StrBuf_Make(&t->SessionMem, text.Length + 1);
  StrBuf_Append(&ev->Payload, text);
}

StrView DisplayPayload(const TuiDisplayEvent& ev) {
  return StrView{ev.Payload.Data, ev.Payload.Length};
}

void QueueText(Tui* t, StrView text) {
  if (text.Length == 0) return;
  if (!HasQueuedDisplayEvents(t)) {
    StreamReveal_Push(&t->Reveal, text);
    t->State = TuiState::Streaming;
    return;
  }

  TuiDisplayEvent* last = &t->DisplayEvents.Data[t->DisplayEvents.Count - 1];
  if (last->Kind != TuiDisplayEventKind::Text) {
    last = QueueDisplayEvent(t, TuiDisplayEventKind::Text);
    SetDisplayPayload(t, last, text);
  } else {
    StrBuf_Append(&last->Payload, text);
  }
}

StrView Truncate(StrView s, u32 maxBytes) {
  if (s.Length <= maxBytes) return s;
  u32 n = maxBytes;
  while (n > 0 && ((unsigned char)s.Data[n] & 0xC0) == 0x80) n--;
  return StrView{s.Data, n};
}

// ---- input layout ----
struct InputLayout {
  StrView Rows[256];
  u32 RowCount;
  u32 CurRow;
  u16 CurCol;
};

u32 InputCellWidth(u32 cp) {
  u32 width = CharWidth(cp);
  return width < 1 ? 1 : width;
}

void LayoutInput(StrView text, u32 cursor, u16 width, InputLayout* out) {
  out->RowCount = 0;
  out->CurRow = 0;
  out->CurCol = 0;
  if (width < 1) width = 1;
  u32 rowStart = 0;
  u16 col = 0;
  u32 i = 0;
  while (true) {
    if (i == cursor) {
      out->CurRow = out->RowCount;
      out->CurCol = col;
    }
    if (i >= text.Length) break;
    if (text.Data[i] == '\n') {
      if (out->RowCount < 256) out->Rows[out->RowCount++] = StrView{text.Data + rowStart, i - rowStart};
      i++;
      rowStart = i;
      col = 0;
      continue;
    }
    u32 jj = i;
    u32 cp = Utf8Decode(text, &jj);
    u32 cw = InputCellWidth(cp);
    if ((u32)col + cw > width) {
      if (out->RowCount < 256) out->Rows[out->RowCount++] = StrView{text.Data + rowStart, i - rowStart};
      rowStart = i;
      col = 0;
      if (i == cursor) {
        out->CurRow = out->RowCount;
        out->CurCol = 0;
      }
    }
    col = (u16)(col + cw);
    i = jj;
  }
  if (out->RowCount < 256) out->Rows[out->RowCount++] = StrView{text.Data + rowStart, text.Length - rowStart};
}

u32 InputTextWidth(StrView text) {
  u32 width = 0;
  u32 i = 0;
  while (i < text.Length) width += InputCellWidth(Utf8Decode(text, &i));
  return width;
}

Style HoverStyle(Style style) {
  if (g_color)
    style.Bg = 235;
  else
    style.Attr |= ATTR_DIM;
  return style;
}

Style RowTextStyle(Style style, bool hovered) {
  if (hovered) return HoverStyle(style);
  return style;
}

const u8 SELECTION_BG = 238;

void ClearTranscriptSelection(Tui* t) {
  if (t->TranscriptSelectionRevision == 0 &&
      t->DragDomain != TuiDragDomain::Transcript)
    return;
  t->TranscriptSelectionRevision = 0;
  t->TranscriptSelectionHash = 0;
  t->SelectionEdgeDirection = 0;
  t->NextSelectionScrollMs = 0;
  if (t->DragDomain == TuiDragDomain::Transcript)
    t->DragDomain = TuiDragDomain::None;
}

void FocusComposer(Tui* t) { ClearTranscriptSelection(t); }

bool TranscriptPointsDiffer(const Tui* t) {
  return t->TranscriptAnchorRow != t->TranscriptFocusRow ||
         t->TranscriptAnchorCell != t->TranscriptFocusCell;
}

struct TranscriptSelection {
  u32 FirstRow;
  u32 LastRow;
  u16 FirstCell;
  u16 LastCell;
};

bool NormalizeTranscriptSelection(const Tui* t, TranscriptSelection* selection) {
  if (!TranscriptPointsDiffer(t)) return false;
  bool forward = t->TranscriptAnchorRow < t->TranscriptFocusRow ||
                 (t->TranscriptAnchorRow == t->TranscriptFocusRow &&
                  t->TranscriptAnchorCell <= t->TranscriptFocusCell);
  if (forward) {
    selection->FirstRow = t->TranscriptAnchorRow;
    selection->FirstCell = t->TranscriptAnchorCell;
    selection->LastRow = t->TranscriptFocusRow;
    selection->LastCell = t->TranscriptFocusCell;
  } else {
    selection->FirstRow = t->TranscriptFocusRow;
    selection->FirstCell = t->TranscriptFocusCell;
    selection->LastRow = t->TranscriptAnchorRow;
    selection->LastCell = t->TranscriptAnchorCell;
  }
  selection->LastCell++;
  return true;
}

bool TranscriptRangeForRow(const TranscriptSelection* selection, u32 row,
                           const DisplayRowLayout* layout,
                           DisplayRowRange* out) {
  if (!selection || !layout || layout->Columns == 0 ||
      row < selection->FirstRow || row > selection->LastRow)
    return false;

  u16 start = 0;
  u16 end = layout->ContentEnd;
  if (selection->FirstRow == selection->LastRow) {
    start = selection->FirstCell;
    end = selection->LastCell;
  } else if (row == selection->FirstRow) {
    start = selection->FirstCell;
    if (start >= end) return false;
  } else if (row == selection->LastRow) {
    end = selection->LastCell;
  }
  *out = LineModel_NormalizeDisplayRowRange(layout, start, end);
  return out->Start != out->End;
}

u64 MixSelectionHash(u64 hash, u64 value) {
  for (u32 i = 0; i < 8; i++) {
    hash ^= (u8)(value >> (i * 8));
    hash *= 1099511628211ull;
  }
  return hash;
}

u64 ComputeTranscriptSelectionHash(const Tui* t) {
  TranscriptSelection selection;
  if (!NormalizeTranscriptSelection(t, &selection)) return 0;
  u64 hash = 1469598103934665603ull;
  for (u32 row = selection.FirstRow; row <= selection.LastRow; row++) {
    const SpanLine* line = LineModel_At(&t->Model, row);
    if (!line) return 0;
    DisplayRowLayout layout = LineModel_LayoutDisplayRow(line, t->Model.Width);
    DisplayRowRange range;
    if (!TranscriptRangeForRow(&selection, row, &layout, &range)) continue;
    hash = MixSelectionHash(hash, row);
    hash = MixSelectionHash(hash, range.Start);
    hash = MixSelectionHash(hash, range.End);
    hash = MixSelectionHash(
        hash, LineModel_HashDisplayRow(&layout, range.Start, range.End));
  }
  return hash;
}

void RefreshTranscriptSelectionHash(Tui* t) {
  t->TranscriptSelectionHash = ComputeTranscriptSelectionHash(t);
}

bool ValidateTranscriptSelection(Tui* t) {
  if (t->TranscriptSelectionRevision == 0) return false;
  if (t->TranscriptSelectionRevision != LineModel_DisplayRevision(&t->Model) ||
      (TranscriptPointsDiffer(t) &&
       t->TranscriptSelectionHash != ComputeTranscriptSelectionHash(t))) {
    ClearTranscriptSelection(t);
    return false;
  }
  return true;
}

bool GetTranscriptSelection(Tui* t, TranscriptSelection* selection) {
  return ValidateTranscriptSelection(t) &&
         NormalizeTranscriptSelection(t, selection);
}

void ApplySelectedCells(Screen* s, u16 y, DisplayRowRange range) {
  if (y >= s->Rows || range.Start >= range.End) return;
  if (range.End > s->Cols) range.End = s->Cols;
  for (u16 x = range.Start; x < range.End; x++) {
    Style* style = &s->Back[(u32)y * s->Cols + x].St;
    if (g_color)
      style->Bg = SELECTION_BG;
    else if (style->Attr & ATTR_UNDERLINE)
      style->Attr |= ATTR_REVERSE;
    else
      style->Attr |= ATTR_UNDERLINE;
  }
}

void RenderSpanLine(Tui* t, u16 y, u32 row, const SpanLine* ln, u16 cols, bool hovered,
                    const TranscriptSelection* selection) {
  Screen* s = &t->Scr;
  if (cols && hovered) {
    Style fill = HoverStyle(StyleDefault());
    Screen_Fill(s, 0, y, (u16)(cols - 1), y, fill);
  }

  DisplayRowLayout layout = LineModel_LayoutDisplayRow(ln, cols);
  DisplayRowIterator iterator = {};
  DisplayRowRun run;
  while (LineModel_NextDisplayRowRun(&layout, &iterator, &run))
    Screen_PutText(s, run.Column, y, run.Text, RowTextStyle(run.St, hovered),
                   (u16)(run.Column + run.CellWidth));

  DisplayRowRange selected;
  if (TranscriptRangeForRow(selection, row, &layout, &selected))
    ApplySelectedCells(s, y, selected);
}

void ComposeStatus(Tui* t, u16 statusY, u16 cols) {
  // No background fill: this row reads as ambient/muted text on the default
  // background, matching the sticky cwd line above the composer box.
  Style st = St(g_color ? 244 : COL_DEFAULT, ATTR_NONE);

  // Left: agent state.
  char left[256];
  u32 ll = 0;
  if (t->OperationActive) {
    Put(left, sizeof(left), &ll, kSpinner[t->Spinner % 10]);
    Put(left, sizeof(left), &ll, " ");
    for (u32 i = 0; i < t->OperationLabel.Length && ll < sizeof(left) - 1; i++)
      left[ll++] = t->OperationLabel.Data[i];
    if (t->OperationStatus.Length) {
      Put(left, sizeof(left), &ll, ": ");
      for (u32 i = 0; i < t->OperationStatus.Length && ll < sizeof(left) - 1; i++)
        left[ll++] = t->OperationStatus.Data[i];
    }
  } else if (t->State == TuiState::Idle) {
    Put(left, sizeof(left), &ll, "ready");
  } else {
    Put(left, sizeof(left), &ll, kSpinner[t->Spinner % 10]);
    Put(left, sizeof(left), &ll, " ");
    if (t->State == TuiState::Tool) {
      Put(left, sizeof(left), &ll, "running ");
      for (u32 i = 0; i < t->StateTool.Length && ll < sizeof(left) - 1; i++) left[ll++] = t->StateTool.Data[i];
    } else if (t->State == TuiState::Streaming) {
      Put(left, sizeof(left), &ll, "responding");
    } else {
      Put(left, sizeof(left), &ll, "thinking");
    }
  }

  // Right (right-aligned): model name + token usage.
  char right[256];
  u32 rl = 0;
  for (u32 i = 0; i < t->ModelName.Length && rl < sizeof(right) - 1; i++) right[rl++] = t->ModelName.Data[i];
  if (t->Use.Input || t->Use.Output) {
    Put(right, sizeof(right), &rl, "  \xe2\x86\x91");
    PutScaled(right, sizeof(right), &rl, t->Use.Input);
    Put(right, sizeof(right), &rl, " \xe2\x86\x93");
    PutScaled(right, sizeof(right), &rl, t->Use.Output);
  }

  StrView rightView{right, rl};
  u32 rightW = TextWidth(rightView);
  u16 rightX = (u32)cols > rightW ? (u16)(cols - rightW) : 0;

  // Draw the state on the left, clipped so it never overruns the right block.
  Screen_PutText(&t->Scr, 1, statusY, StrView{left, ll}, st,
                 rightX ? rightX : cols);
  Screen_PutText(&t->Scr, rightX, statusY, rightView, st, cols);
}

// Composer input box: a rounded border (no background fill) drawn around the
// input row(s). `innerTop`/`innerH` is the text area; the border occupies the
// row just above and below it plus the first/last column of every row.
void DrawInputBox(Tui* t, u16 topY, u16 bottomY, u16 innerTop, u16 innerH, u16 cols) {
  if (cols < 2) return;
  Style border = St(g_color ? 250 : COL_DEFAULT, ATTR_NONE);  // default bg: border only, no fill

  const u32 kMaxRule = 512;
  char rule[kMaxRule * 3];
  u32 rl = 0;
  u16 innerW = (u16)(cols - 2);
  u16 ruleLen = innerW < kMaxRule ? innerW : kMaxRule;
  for (u16 i = 0; i < ruleLen; i++) Put(rule, sizeof(rule), &rl, "\xe2\x94\x80");
  StrView ruleView{rule, rl};

  Screen_PutText(&t->Scr, 0, topY, SVL("\xe2\x95\xad"), border, cols);
  Screen_PutText(&t->Scr, 1, topY, ruleView, border, (u16)(cols - 1));
  Screen_PutText(&t->Scr, (u16)(cols - 1), topY, SVL("\xe2\x95\xae"), border, cols);

  Screen_PutText(&t->Scr, 0, bottomY, SVL("\xe2\x95\xb0"), border, cols);
  Screen_PutText(&t->Scr, 1, bottomY, ruleView, border, (u16)(cols - 1));
  Screen_PutText(&t->Scr, (u16)(cols - 1), bottomY, SVL("\xe2\x95\xaf"), border, cols);

  for (u16 y = innerTop; y < innerTop + innerH; y++) {
    Screen_PutText(&t->Scr, 0, y, SVL("\xe2\x94\x82"), border, cols);
    Screen_PutText(&t->Scr, (u16)(cols - 1), y, SVL("\xe2\x94\x82"), border, cols);
  }
}

void HighlightInputSelection(Tui* t, u16 y, StrView row, StrView text, u16 x) {
  u32 selectionStart, selectionEnd;
  if (!LineEdit_SelectionBounds(&t->Edit, &selectionStart, &selectionEnd)) return;
  u32 rowStart = (u32)(row.Data - text.Data);
  u32 rowEnd = rowStart + row.Length;
  u32 start = selectionStart > rowStart ? selectionStart : rowStart;
  u32 end = selectionEnd < rowEnd ? selectionEnd : rowEnd;
  if (start < end) {
    u16 startCol = (u16)InputTextWidth(StrView{text.Data + rowStart, start - rowStart});
    u16 endCol = (u16)InputTextWidth(StrView{text.Data + rowStart, end - rowStart});
    ApplySelectedCells(&t->Scr, y,
                       DisplayRowRange{(u16)(x + startCol), (u16)(x + endCol)});
  }
  if (rowEnd < text.Length && text.Data[rowEnd] == '\n' && selectionStart <= rowEnd &&
      selectionEnd > rowEnd) {
    u16 newlineCol = (u16)InputTextWidth(row);
    ApplySelectedCells(
        &t->Scr, y,
        DisplayRowRange{(u16)(x + newlineCol), (u16)(x + newlineCol + 1u)});
  }
}

// Sticky working-directory + git-branch line, drawn on the row just above the
// prompt. Muted so it reads as ambient context, not transcript content.
void ComposeCwd(Tui* t, u16 cwdY, u16 cols) {
  if (t->Cwd.Length == 0) return;
  Style st = St(g_color ? 244 : COL_DEFAULT, ATTR_NONE);
  u16 x = Screen_PutText(&t->Scr, 1, cwdY, t->Cwd, st, cols);
  if (t->Branch.Length) {
    x = Screen_PutText(&t->Scr, x, cwdY, SVL("  (\xe2\x8e\x87 "), st, cols);
    x = Screen_PutText(&t->Scr, x, cwdY, t->Branch, st, cols);
    Screen_PutText(&t->Scr, x, cwdY, SVL(")"), st, cols);
  }
}

struct ViewportAnchor {
  u64 EntryId;
  u32 EntryRowOffset;
  u32 ScreenRow;
};

u32 MaxTop(u32 count, u16 paneH) { return count > paneH ? count - paneH : 0; }

u16 ScrollbarX(const Tui* t) { return t->Scr.Cols ? (u16)(t->Scr.Cols - 1) : 0; }

void ResetScrollbar(Tui* t) {
  t->ScrollbarVisible = false;
  t->ScrollbarDragging = false;
  t->LastScrollbarThumbTop = 0;
  t->LastScrollbarThumbH = 0;
  t->ScrollbarDragOffset = 0;
}

ViewportAnchor CaptureViewportAnchor(const Tui* t) {
  ViewportAnchor anchor = {};
  if (t->Follow) return anchor;

  u32 count = LineModel_Count(&t->Model);
  u32 maxTop = MaxTop(count, t->LastPaneH);
  u32 top = t->Top < maxTop ? t->Top : maxTop;
  for (u32 row = top; row < count; row++) {
    u64 id = LineModel_RowEntryId(&t->Model, row);
    u32 first = 0;
    if (id == 0 || !LineModel_FindEntryRows(&t->Model, id, &first, nullptr)) continue;
    anchor.EntryId = id;
    anchor.EntryRowOffset = row - first;
    anchor.ScreenRow = row - top;
    break;
  }
  return anchor;
}

void RestoreViewportAnchor(Tui* t, ViewportAnchor anchor) {
  if (t->Follow) return;

  u32 first = 0;
  u32 count = 0;
  if (anchor.EntryId != 0 &&
      LineModel_FindEntryRows(&t->Model, anchor.EntryId, &first, &count)) {
    u32 offset = anchor.EntryRowOffset < count ? anchor.EntryRowOffset : count - 1;
    u32 target = first + offset;
    t->Top = target > anchor.ScreenRow ? target - anchor.ScreenRow : 0;
  }

  u32 lineCount = LineModel_Count(&t->Model);
  u32 maxTop = MaxTop(lineCount, t->LastPaneH);
  if (t->Top > maxTop) t->Top = maxTop;
}

struct ScrollbarGeometry {
  u16 ThumbTop;
  u16 ThumbH;
};

ScrollbarGeometry CalcScrollbarGeometry(u32 count, u16 paneH, u32 top) {
  ScrollbarGeometry out = {};
  u32 maxTop = count - paneH;
  u32 thumbH = (u32)(((u64)paneH * paneH) / count);
  if (thumbH < 1) thumbH = 1;
  if (thumbH > paneH) thumbH = paneH;
  u32 travel = paneH - thumbH;
  u32 clampedTop = top < maxTop ? top : maxTop;
  u32 thumbTop =
      travel ? (u32)(((u64)clampedTop * travel + maxTop / 2u) / maxTop) : 0;
  out.ThumbTop = (u16)thumbTop;
  out.ThumbH = (u16)thumbH;
  return out;
}

void DrawScrollbar(Tui* t, u16 paneH, u32 count, u32 top) {
  t->LastScrollbarThumbTop = 0;
  t->LastScrollbarThumbH = 0;
  if (!t->ScrollbarVisible || paneH == 0 || count <= paneH) return;

  ScrollbarGeometry geometry = CalcScrollbarGeometry(count, paneH, top);
  t->LastScrollbarThumbTop = geometry.ThumbTop;
  t->LastScrollbarThumbH = geometry.ThumbH;

  u16 x = ScrollbarX(t);
  Style track = g_color ? StyleMake(238, COL_DEFAULT, ATTR_NONE)
                        : StyleMake(COL_DEFAULT, COL_DEFAULT, ATTR_DIM);
  Style thumb = g_color ? StyleMake(250, COL_DEFAULT, ATTR_NONE)
                        : StyleMake(COL_DEFAULT, COL_DEFAULT, ATTR_BOLD);
  for (u16 y = 0; y < paneH; y++)
    Screen_PutChar(&t->Scr, x, y, 0x2595, track);
  for (u16 y = 0; y < geometry.ThumbH; y++)
    Screen_PutChar(&t->Scr, x, (u16)(geometry.ThumbTop + y), 0x2595, thumb);
}

void ApplyDisplayEvent(Tui* t, const TuiDisplayEvent& ev) {
  switch (ev.Kind) {
    case TuiDisplayEventKind::MessageEnd:
      if (t->Model.HasAssistant) LineModel_EndAssistant(&t->Model);
      t->Use = ev.Use;
      if (ev.Stop == StopReason::Error)
        LineModel_AddNotice(&t->Model, EntryKind::Error, DisplayPayload(ev));
      else if (ev.Stop == StopReason::Aborted)
        LineModel_AddNotice(&t->Model, EntryKind::Info, SVL("Interrupted by user."));
      break;
    case TuiDisplayEventKind::ToolStart:
      LineModel_AddTool(&t->Model, ev.ToolCallId, ev.ToolName, ev.ToolArgsJson);
      t->State = TuiState::Tool;
      t->StateTool = ev.ToolName;
      break;
    case TuiDisplayEventKind::ToolEnd:
      {
        StrView payload = DisplayPayload(ev);
        if (!LineModel_FinishTool(&t->Model, ev.ToolCallId, payload, Truncate(payload, 1500),
                                  ev.IsError))
          LineModel_AddNotice(&t->Model, EntryKind::Error,
                              SVL("tool result did not match a pending call"));
      }
      t->State = TuiState::Thinking;
      break;
    case TuiDisplayEventKind::User:
      if (t->Model.HasAssistant) LineModel_EndAssistant(&t->Model);
      LineModel_AddUser(&t->Model, DisplayPayload(ev), ev.Timestamp);
      break;
    case TuiDisplayEventKind::Notice:
      if (t->Model.HasAssistant) LineModel_EndAssistant(&t->Model);
      LineModel_AddNotice(&t->Model, EntryKind::Info, DisplayPayload(ev));
      break;
    case TuiDisplayEventKind::Error:
      if (t->Model.HasAssistant) LineModel_EndAssistant(&t->Model);
      LineModel_AddNotice(&t->Model, EntryKind::Error, DisplayPayload(ev));
      break;
    case TuiDisplayEventKind::AgentEnd:
      t->State = t->AgentRunning ? TuiState::Thinking : TuiState::Idle;
      break;
    case TuiDisplayEventKind::Text: break;
  }
}

bool PumpDisplay(Tui* t, u64 nowMs) {
  ViewportAnchor anchor = CaptureViewportAnchor(t);
  bool changed = false;

  for (;;) {
    if (StreamReveal_HasPending(&t->Reveal)) {
      if (!t->Model.HasAssistant) {
        LineModel_BeginAssistant(&t->Model);
        t->State = TuiState::Streaming;
      }
      StrView next =
          StreamReveal_NextFrame(&t->Reveal, nowMs, HasQueuedDisplayEvents(t));
      if (next.Length) {
        LineModel_AppendAssistant(&t->Model, next);
        changed = true;
      }
      if (StreamReveal_HasPending(&t->Reveal)) {
        RestoreViewportAnchor(t, anchor);
        return changed;
      }
    }

    if (!HasQueuedDisplayEvents(t)) break;
    TuiDisplayEvent* ev = &t->DisplayEvents.Data[t->DisplayHead++];
    if (ev->Kind == TuiDisplayEventKind::Text) {
      StreamReveal_Adopt(&t->Reveal, ev->Payload);
      t->State = TuiState::Streaming;
      continue;
    }
    ApplyDisplayEvent(t, *ev);
    changed = true;
  }

  CompactDisplayEvents(t);
  RestoreViewportAnchor(t, anchor);
  return changed;
}

bool AdvanceSelectionTick(Tui* t, u64 nowMs);

void DrawPanelBorder(Tui* t, u16 x0, u16 y0, u16 x1, u16 y1, Style style) {
  if (x1 <= x0 || y1 <= y0) return;
  Screen_Fill(&t->Scr, x0, y0, x1, y1, style);
  Screen_PutText(&t->Scr, x0, y0, SVL("\xe2\x95\xad"), style, (u16)(x0 + 1));
  Screen_PutText(&t->Scr, x1, y0, SVL("\xe2\x95\xae"), style, (u16)(x1 + 1));
  Screen_PutText(&t->Scr, x0, y1, SVL("\xe2\x95\xb0"), style, (u16)(x0 + 1));
  Screen_PutText(&t->Scr, x1, y1, SVL("\xe2\x95\xaf"), style, (u16)(x1 + 1));
  for (u16 x = (u16)(x0 + 1); x < x1; x++) {
    Screen_PutText(&t->Scr, x, y0, SVL("\xe2\x94\x80"), style, (u16)(x + 1));
    Screen_PutText(&t->Scr, x, y1, SVL("\xe2\x94\x80"), style, (u16)(x + 1));
  }
  for (u16 y = (u16)(y0 + 1); y < y1; y++) {
    Screen_PutText(&t->Scr, x0, y, SVL("\xe2\x94\x82"), style, (u16)(x0 + 1));
    Screen_PutText(&t->Scr, x1, y, SVL("\xe2\x94\x82"), style, (u16)(x1 + 1));
  }
}

void ComposeModal(Tui* t, u16 cols, u16 topBorderY) {
  if (t->ModalKind == TuiModalKind::None || t->ModalResult != TuiModalResult::Pending)
    return;

  if (topBorderY < 4) return;
  u16 panelW = cols > 76 ? 72 : (u16)(cols - 2);
  u16 desiredH =
      t->ModalKind == TuiModalKind::Select
          ? (u16)((t->ModalOptionCount < 12 ? t->ModalOptionCount : 12) + 3)
          : 6;
  u16 panelH = desiredH < topBorderY ? desiredH : topBorderY;
  if (panelH < 4) return;
  u16 x0 = 1;
  u16 y0 = (u16)(topBorderY - panelH);
  u16 x1 = (u16)(x0 + panelW - 1);
  u16 y1 = (u16)(topBorderY - 1);
  bool compactText = t->ModalKind == TuiModalKind::Text && panelH < 5;
  Style panel = St(g_color ? 252 : COL_DEFAULT, ATTR_NONE);
  DrawPanelBorder(t, x0, y0, x1, y1, panel);

  Style path = panel;
  path.Attr |= ATTR_DIM;
  u16 titleX =
      Screen_PutText(&t->Scr, (u16)(x0 + 2), (u16)(y0 + 1),
                     SVL("Commands  /  "), path, (u16)(x1 - 1));
  Style title = panel;
  title.Attr |= ATTR_BOLD;
  Screen_PutText(&t->Scr, titleX, (u16)(y0 + 1),
                 compactText ? t->ModalPrompt : t->ModalTitle, title,
                 (u16)(x1 - 1));

  if (t->ModalKind == TuiModalKind::Select) {
    u32 visible = panelH > 3 ? panelH - 3 : 1;
    if (t->ModalSelected < t->ModalFirst) t->ModalFirst = t->ModalSelected;
    if (t->ModalSelected >= t->ModalFirst + visible)
      t->ModalFirst = t->ModalSelected - visible + 1;
    u32 maxFirst = t->ModalOptionCount > visible ? t->ModalOptionCount - visible : 0;
    if (t->ModalFirst > maxFirst) t->ModalFirst = maxFirst;

    for (u32 row = 0; row < visible; row++) {
      u32 index = t->ModalFirst + row;
      if (index >= t->ModalOptionCount) break;
      u16 y = (u16)(y0 + 2 + row);
      bool selected = index == t->ModalSelected;
      Style option = panel;
      if (selected) option.Attr |= ATTR_REVERSE;
      if (selected) Screen_Fill(&t->Scr, (u16)(x0 + 1), y, (u16)(x1 - 1), y, option);
      Screen_PutText(&t->Scr, (u16)(x0 + 2), y, selected ? SVL("\xe2\x80\xba ") : SVL("  "),
                     option, (u16)(x1 - 1));
      Screen_PutText(&t->Scr, (u16)(x0 + 4), y, t->ModalOptions[index], option,
                     (u16)(x1 - 1));
    }
    Screen_SetCursor(&t->Scr, false, 0, 0);
    return;
  }

  Style prompt = panel;
  prompt.Attr |= ATTR_DIM;
  if (!compactText)
    Screen_PutText(&t->Scr, (u16)(x0 + 2), (u16)(y0 + 2), t->ModalPrompt,
                   prompt, (u16)(x1 - 1));
  u16 editY = (u16)(y1 - 1);
  Style edit = panel;
  edit.Attr |= ATTR_REVERSE;
  Screen_Fill(&t->Scr, (u16)(x0 + 2), editY, (u16)(x1 - 2), editY, edit);
  StrView text = LineEdit_Text(&t->ModalEdit);
  Screen_PutText(&t->Scr, (u16)(x0 + 3), editY, text, edit, (u16)(x1 - 2));
  u32 cursorW = TextWidth(StrView{text.Data, LineEdit_Cursor(&t->ModalEdit)});
  u16 cursorX = (u16)(x0 + 3 + cursorW);
  if (cursorX >= x1 - 2) cursorX = (u16)(x1 - 3);
  Screen_SetCursor(&t->Scr, true, cursorX, editY);
}

void ComposeCommandPalette(Tui* t, StrView query, u32 matchCount, u16 topBorderY,
                           u16 paletteH, u16 cols) {
  if (paletteH < 4 || topBorderY < paletteH) return;

  u16 y0 = (u16)(topBorderY - paletteH);
  u16 y1 = (u16)(topBorderY - 1);
  u16 panelW = cols > 76 ? 72 : (u16)(cols - 2);
  u16 x0 = 1;
  u16 x1 = (u16)(x0 + panelW - 1);
  Style panel = St(g_color ? 252 : COL_DEFAULT, ATTR_NONE);
  DrawPanelBorder(t, x0, y0, x1, y1, panel);

  Style title = panel;
  title.Attr |= ATTR_BOLD;
  Screen_PutText(&t->Scr, (u16)(x0 + 2), (u16)(y0 + 1), SVL("Commands"),
                 title, x1);

  u32 visible = paletteH - 3;
  if (matchCount == 0) {
    t->CommandPaletteSelected = 0;
    t->CommandPaletteFirst = 0;
    Style empty = panel;
    empty.Attr |= ATTR_DIM;
    Screen_PutText(&t->Scr, (u16)(x0 + 2), (u16)(y0 + 2),
                   SVL("No matching commands"), empty, x1);
    return;
  }

  if (t->CommandPaletteSelected >= matchCount) t->CommandPaletteSelected = 0;
  if (t->CommandPaletteSelected < t->CommandPaletteFirst)
    t->CommandPaletteFirst = t->CommandPaletteSelected;
  if (t->CommandPaletteSelected >= t->CommandPaletteFirst + visible)
    t->CommandPaletteFirst = t->CommandPaletteSelected - visible + 1;
  u32 maxFirst = matchCount > visible ? matchCount - visible : 0;
  if (t->CommandPaletteFirst > maxFirst) t->CommandPaletteFirst = maxFirst;

  for (u32 row = 0; row < visible; row++) {
    u32 matchIndex = t->CommandPaletteFirst + row;
    const TuiCommandSpec* command = CommandMatchAt(t, query, matchIndex);
    if (!command) break;
    u16 y = (u16)(y0 + 2 + row);
    bool selected = matchIndex == t->CommandPaletteSelected;
    Style option = panel;
    if (selected) {
      option.Attr |= ATTR_REVERSE;
      Screen_Fill(&t->Scr, (u16)(x0 + 1), y, (u16)(x1 - 1), y, option);
    }

    u16 x = Screen_PutText(&t->Scr, (u16)(x0 + 2), y,
                           selected ? SVL("\xe2\x80\xba ") : SVL("  "), option, x1);
    Style name = option;
    name.Attr |= ATTR_BOLD;
    x = Screen_PutText(&t->Scr, x, y, SVL("/"), name, x1);
    x = Screen_PutText(&t->Scr, x, y, command->Name, name, x1);

    u16 descriptionX = (u16)(x0 + 18);
    if (descriptionX < x + 2) descriptionX = (u16)(x + 2);
    Style description = option;
    description.Attr |= ATTR_DIM;
    Screen_PutText(&t->Scr, descriptionX, y, command->Description, description, x1);
  }
}

struct TranscriptLayout {
  u16 Cols;
  u32 Count;
  u32 Top;
};

TranscriptLayout LayoutTranscript(Tui* t, u16 cols, u16 paneH, bool geometryChanged) {
  ViewportAnchor anchor = CaptureViewportAnchor(t);
  if (geometryChanged) t->ScrollbarDragging = false;

  u16 contentCols = t->ScrollbarVisible ? (u16)(cols - 1) : cols;
  LineModel_SetWidth(&t->Model, contentCols);
  u32 count = LineModel_Count(&t->Model);
  if (!t->ScrollbarVisible && count > paneH) {
    t->ScrollbarVisible = true;
    contentCols = (u16)(cols - 1);
    LineModel_SetWidth(&t->Model, contentCols);
    count = LineModel_Count(&t->Model);
  } else if (t->ScrollbarVisible && LineModel_FitsAtWidth(&t->Model, cols, paneH)) {
    ResetScrollbar(t);
    contentCols = cols;
    LineModel_SetWidth(&t->Model, contentCols);
    count = LineModel_Count(&t->Model);
  }

  t->LastPaneH = paneH;
  RestoreViewportAnchor(t, anchor);
  u32 maxTop = MaxTop(count, paneH);
  u32 top = t->Follow ? maxTop : t->Top;
  t->LastTop = top;
  return TranscriptLayout{contentCols, count, top};
}

void ComposeAtSize(Tui* t, u16 cols, u16 rows, u64 nowMs) {
  bool sizeChanged = cols != t->Scr.Cols || rows != t->Scr.Rows;
  if (sizeChanged) {
    ClearTranscriptSelection(t);
    Screen_Resize(&t->Scr, cols, rows);
  }
  t->LastCols = cols;
  if (cols < 6 || rows < 6) {
    ViewportAnchor anchor = CaptureViewportAnchor(t);
    ResetScrollbar(t);
    t->LastPaneH = 0;
    t->LastInputH = 0;
    t->LastTop = 0;
    LineModel_SetWidth(&t->Model, cols);
    RestoreViewportAnchor(t, anchor);
    Screen_BeginFrame(&t->Scr);
    return;
  }

  PumpDisplay(t, nowMs);
  AdvanceSelectionTick(t, nowMs);
  if (t->State != TuiState::Idle || t->OperationActive) t->Spinner++;

  // Input box layout: left/right border column + one padding column each side.
  u16 textW = (u16)(cols > 4 ? cols - 4 : 1);
  InputLayout il;
  LayoutInput(LineEdit_Text(&t->Edit), LineEdit_Cursor(&t->Edit), textW, &il);
  u16 maxInputH = (u16)(rows / 2);
  if (maxInputH < 1) maxInputH = 1;
  if (maxInputH > 10) maxInputH = 10;
  u16 inputH = (u16)(il.RowCount < maxInputH ? il.RowCount : maxInputH);
  if (inputH < 1) inputH = 1;

  // Reserve the bottom row for the status bar, a top/bottom border row for the
  // composer box, and the row just above the box for the sticky cwd/branch
  // line; keep at least one transcript row (rows >= 6 is guaranteed above).
  u16 statusY = (u16)(rows - 1);
  u16 maxInput = (u16)(rows - 5);
  if (inputH > maxInput) inputH = maxInput;
  if (inputH < 1) inputH = 1;
  u16 bottomBorderY = (u16)(statusY - 1);
  u16 inputTop = (u16)(bottomBorderY - inputH);
  u16 topBorderY = (u16)(inputTop - 1);
  StrView commandQuery = {};
  bool commandPalette = CommandPaletteQuery(t, &commandQuery);
  u32 commandMatches = commandPalette ? CommandMatchCount(t, commandQuery) : 0;
  u16 paletteH = 0;
  if (commandPalette) {
    u32 visible = commandMatches ? commandMatches : 1;
    if (visible > 6) visible = 6;
    u16 desired = (u16)(visible + 3);
    if (topBorderY >= 4) paletteH = desired < topBorderY ? desired : topBorderY;
  }
  u16 cwdY = (u16)(topBorderY - 1);
  u16 paneH = cwdY;  // rows [0, cwdY)
  t->LastInputTop = inputTop;
  t->LastInputH = inputH;

  TranscriptLayout transcript =
      LayoutTranscript(t, cols, paneH, sizeChanged || paneH != t->LastPaneH);

  Screen_BeginFrame(&t->Scr);

  // Transcript.
  if (t->HasMouse && t->MouseY < paneH &&
      (!t->ScrollbarVisible || t->MouseX != ScrollbarX(t))) {
    const SpanLine* hoverLine = LineModel_At(&t->Model, transcript.Top + t->MouseY);
    t->HoveredEntryId = hoverLine && hoverLine->IsEntryStart ? hoverLine->EntryId : 0;
  } else {
    t->HoveredEntryId = 0;
  }
  TranscriptSelection selection;
  bool hasSelection = GetTranscriptSelection(t, &selection);

  for (u16 r = 0; r < paneH; r++) {
    u32 idx = transcript.Top + r;
    if (idx >= transcript.Count) break;
    const SpanLine* line = LineModel_At(&t->Model, idx);
    bool hovered =
        line->IsEntryStart && line->EntryId != 0 && line->EntryId == t->HoveredEntryId;
    RenderSpanLine(t, r, idx, line, transcript.Cols, hovered,
                   hasSelection ? &selection : nullptr);
  }
  DrawScrollbar(t, paneH, transcript.Count, transcript.Top);

  // Status.
  ComposeStatus(t, statusY, cols);

  // Composer box (scroll within if taller than the box).
  DrawInputBox(t, topBorderY, bottomBorderY, inputTop, inputH, cols);
  u32 firstRow = il.CurRow >= inputH ? il.CurRow - inputH + 1 : 0;
  if (t->DragDomain == TuiDragDomain::Composer) {
    u32 maxFirstRow = il.RowCount > inputH ? il.RowCount - inputH : 0;
    firstRow = t->LastInputFirstRow < maxFirstRow ? t->LastInputFirstRow : maxFirstRow;
  }
  t->LastInputFirstRow = firstRow;
  StrView inputText = LineEdit_Text(&t->Edit);
  for (u16 i = 0; i < inputH; i++) {
    u32 rr = firstRow + i;
    if (rr >= il.RowCount) break;
    Screen_PutText(&t->Scr, 2, (u16)(inputTop + i), il.Rows[rr], StyleDefault(), (u16)(cols - 1));
    HighlightInputSelection(t, (u16)(inputTop + i), il.Rows[rr], inputText, 2);
  }
  u16 curScreenRow = (u16)(inputTop + (il.CurRow - firstRow));
  u16 curScreenCol = (u16)(2 + il.CurCol);
  if (curScreenCol >= (u16)(cols - 1)) curScreenCol = (u16)(cols - 2);
  Screen_SetCursor(&t->Scr, true, curScreenCol, curScreenRow);

  // Sticky context line above the prompt.
  ComposeCwd(t, cwdY, cols);
  ComposeCommandPalette(t, commandQuery, commandMatches, topBorderY, paletteH, cols);
  ComposeModal(t, cols, topBorderY);
}

void Compose(Tui* t) {
  u16 cols = 0, rows = 0;
  Term_Size(&t->Trm, &cols, &rows);
  ComposeAtSize(t, cols, rows, MonotonicMillis());
}

// ---- key handling (lock held) ----
void QueueUserDisplayLocked(Tui* t, StrView line) {
  TuiDisplayEvent* display = QueueDisplayEvent(t, TuiDisplayEventKind::User);
  SetDisplayPayload(t, display, line);
  display->Timestamp = NowMillis();
  t->Follow = true;
}

void DoSubmit(Tui* t) {
  StrView line = LineEdit_Submit(&t->Edit);
  ResetCommandPalette(t);
  if (line.Length == 0) return;
  if (IsSlashCommand(line)) {
    if (!QPush(t, TuiSubmission{TuiSubmissionKind::Command, line})) {
      TuiDisplayEvent* display = QueueDisplayEvent(t, TuiDisplayEventKind::Error);
      SetDisplayPayload(t, display, SVL("Submission queue is full; command was not queued."));
      t->Follow = true;
      Wake(t);
      return;
    }
    if (t->AgentRunning) {
      TuiDisplayEvent* display = QueueDisplayEvent(t, TuiDisplayEventKind::Notice);
      display->Payload = StrBuf_Make(&t->SessionMem, line.Length + 24);
      StrBuf_AppendCStr(&display->Payload, "Queued command: ");
      StrBuf_Append(&display->Payload, line);
      t->Follow = true;
      Wake(t);
    }
    Cond_Signal(&t->SubmitCv);
    return;
  }

  if (t->AgentRunning && t->Ag) {
    QueueUserDisplayLocked(t, line);
    Agent_Steer(t->Ag, line);  // mid-turn submit -> steer the running agent
  } else {
    if (!QPush(t, TuiSubmission{TuiSubmissionKind::Prompt, line})) {
      TuiDisplayEvent* display = QueueDisplayEvent(t, TuiDisplayEventKind::Error);
      SetDisplayPayload(t, display, SVL("Submission queue is full; prompt was not queued."));
      t->Follow = true;
      Wake(t);
      return;
    }
    Cond_Signal(&t->SubmitCv);
  }
}

void SubmitPaletteCommand(Tui* t, StrView query) {
  u32 matchCount = CommandMatchCount(t, query);
  if (matchCount == 0) {
    DoSubmit(t);
    return;
  }
  if (t->CommandPaletteSelected >= matchCount) t->CommandPaletteSelected = 0;
  const TuiCommandSpec* command =
      CommandMatchAt(t, query, t->CommandPaletteSelected);
  if (!command) {
    DoSubmit(t);
    return;
  }
  LineEdit_Clear(&t->Edit);
  LineEdit_InsertText(&t->Edit, SVL("/"));
  LineEdit_InsertText(&t->Edit, command->Name);
  DoSubmit(t);
}

const i32 kWheelStep = 3;  // transcript rows per mouse-wheel notch

u64 HitTestEntry(const Tui* t, u16 x, u16 y) {
  if (y >= t->LastPaneH) return 0;
  if (t->ScrollbarVisible && x == ScrollbarX(t)) return 0;
  const SpanLine* line = LineModel_At(&t->Model, t->LastTop + y);
  return line && line->IsEntryStart ? line->EntryId : 0;
}

bool HitComposer(const Tui* t, u16 x, u16 y) {
  return t->LastInputH != 0 && y >= t->LastInputTop &&
         y < (u16)(t->LastInputTop + t->LastInputH) && x > 0 &&
         (u32)x + 1u < t->LastCols;
}

void SetComposerMouseCursor(Tui* t, u16 x, u16 y, bool extend = false) {
  if (t->LastInputH == 0 || t->LastCols < 6) return;
  u16 screenRow = y;
  if (screenRow < t->LastInputTop) screenRow = t->LastInputTop;
  u16 lastRow = (u16)(t->LastInputTop + t->LastInputH - 1u);
  if (screenRow > lastRow) screenRow = lastRow;
  u32 row = t->LastInputFirstRow + screenRow - t->LastInputTop;
  u32 col = x > 2 ? x - 2u : 0;
  u16 textW = (u16)(t->LastCols - 4u);
  if (col > textW) col = textW;
  LineEdit_SetCursorAt(&t->Edit, textW, row, col, extend);
}

u16 TranscriptMouseCell(const Tui* t, u16 x) {
  return x < t->Model.Width ? x : (u16)(t->Model.Width - 1u);
}

bool TranscriptMousePoint(const Tui* t, u16 x, u16 y, u32* row, u16* cell) {
  u32 count = LineModel_Count(&t->Model);
  if (count == 0 || t->LastPaneH == 0 || y >= t->LastPaneH) return false;
  u32 pointRow = t->LastTop + y;
  if (pointRow < count) {
    *row = pointRow;
    *cell = TranscriptMouseCell(t, x);
    return true;
  }

  *row = count - 1u;
  DisplayRowLayout layout =
      LineModel_LayoutDisplayRow(LineModel_At(&t->Model, *row), t->Model.Width);
  *cell = layout.ContentEnd ? (u16)(layout.ContentEnd - 1u) : 0;
  return true;
}

void UpdateMousePosition(Tui* t, const Key& key) {
  t->MouseX = key.MouseX;
  t->MouseY = key.MouseY;
  t->HasMouse = true;
}

void UpdateMouse(Tui* t, const Key& key) {
  UpdateMousePosition(t, key);
  t->HoveredEntryId = HitTestEntry(t, key.MouseX, key.MouseY);
}

void SetScrollTop(Tui* t, i64 value, u32 maxTop) {
  if (value < 0) value = 0;
  if (value > (i64)maxTop) value = maxTop;
  t->Top = (u32)value;
  t->Follow = (t->Top >= maxTop);
}

void ScrollBy(Tui* t, i32 delta) {
  u32 count = LineModel_Count(&t->Model);
  u32 maxTop = MaxTop(count, t->LastPaneH);
  u32 cur = t->Follow ? maxTop : t->Top;
  SetScrollTop(t, (i64)cur + delta, maxTop);
}

bool IsScrollbarHit(const Tui* t, const Key& key) {
  return t->ScrollbarVisible && t->LastScrollbarThumbH != 0 &&
         key.MouseX == ScrollbarX(t) && key.MouseY < t->LastPaneH;
}

void ScrollToThumb(Tui* t, i32 thumbTop) {
  u32 count = LineModel_Count(&t->Model);
  if (t->LastPaneH == 0 || count <= t->LastPaneH) return;
  u32 travel = t->LastPaneH - t->LastScrollbarThumbH;
  if (travel == 0) return;
  if (thumbTop < 0) thumbTop = 0;
  if (thumbTop > (i32)travel) thumbTop = (i32)travel;
  u32 maxTop = MaxTop(count, t->LastPaneH);
  u32 top = (u32)(((u64)(u32)thumbTop * maxTop + travel / 2u) / travel);
  SetScrollTop(t, top, maxTop);
}

void BeginScrollbarDrag(Tui* t, const Key& key) {
  UpdateMousePosition(t, key);
  t->HoveredEntryId = 0;
  t->PressedEntryId = 0;

  u16 thumbBottom = (u16)(t->LastScrollbarThumbTop + t->LastScrollbarThumbH);
  if (key.MouseY >= t->LastScrollbarThumbTop && key.MouseY < thumbBottom) {
    t->ScrollbarDragOffset = (u16)(key.MouseY - t->LastScrollbarThumbTop);
  } else {
    t->ScrollbarDragOffset = (u16)(t->LastScrollbarThumbH / 2);
    ScrollToThumb(t, (i32)key.MouseY - t->ScrollbarDragOffset);
  }
  t->ScrollbarDragging = true;
}

void DragScrollbar(Tui* t, const Key& key) {
  UpdateMousePosition(t, key);
  t->HoveredEntryId = 0;
  t->PressedEntryId = 0;
  ScrollToThumb(t, (i32)key.MouseY - t->ScrollbarDragOffset);
}

void FinishModal(Tui* t, TuiModalResult result) {
  t->ModalResult = result;
  Cond_Broadcast(&t->ModalCv);
}

void RequestStopLocked(Tui* t) {
  t->AbortFlag = 1;
  t->OperationAbortFlag = 1;
  t->Quit = true;
  t->Stop = true;
  if (t->ModalResult == TuiModalResult::Pending)
    t->ModalResult = TuiModalResult::Cancelled;
  Cond_Signal(&t->SubmitCv);
  Cond_Broadcast(&t->ModalCv);
  Wake(t);
}

void HandleSelectKey(Tui* t, const Key& k) {
  switch (k.Kind) {
    case KeyKind::Up:
      if (t->ModalSelected > 0) t->ModalSelected--;
      break;
    case KeyKind::Down:
      if (t->ModalSelected + 1 < t->ModalOptionCount) t->ModalSelected++;
      break;
    case KeyKind::Home: t->ModalSelected = 0; break;
    case KeyKind::End:
      if (t->ModalOptionCount) t->ModalSelected = t->ModalOptionCount - 1;
      break;
    case KeyKind::Enter: FinishModal(t, TuiModalResult::Confirmed); break;
    case KeyKind::Backspace: FinishModal(t, TuiModalResult::Back); break;
    case KeyKind::Escape: FinishModal(t, TuiModalResult::Cancelled); break;
    case KeyKind::CtrlC:
      if (t->OperationActive) t->OperationAbortFlag = 1;
      FinishModal(t, TuiModalResult::Cancelled);
      break;
    default: break;
  }
}

void HandleTextKey(Tui* t, const Key& k) {
  switch (k.Kind) {
    case KeyKind::Char:
      if (!k.Alt && k.Codepoint >= 0x20) LineEdit_InsertChar(&t->ModalEdit, k.Codepoint);
      break;
    case KeyKind::Enter:
      if (!LineEdit_Empty(&t->ModalEdit)) {
        t->ModalTextResult = StrView_Dup(&t->SessionMem, LineEdit_Text(&t->ModalEdit));
        FinishModal(t, TuiModalResult::Confirmed);
      }
      break;
    case KeyKind::Backspace:
      if (LineEdit_Empty(&t->ModalEdit))
        FinishModal(t, TuiModalResult::Back);
      else
        LineEdit_Backspace(&t->ModalEdit);
      break;
    case KeyKind::Delete: LineEdit_Delete(&t->ModalEdit); break;
    case KeyKind::Left: LineEdit_Left(&t->ModalEdit); break;
    case KeyKind::Right: LineEdit_Right(&t->ModalEdit); break;
    case KeyKind::Home: LineEdit_Home(&t->ModalEdit); break;
    case KeyKind::End: LineEdit_End(&t->ModalEdit); break;
    case KeyKind::WordLeft: LineEdit_WordLeft(&t->ModalEdit); break;
    case KeyKind::WordRight: LineEdit_WordRight(&t->ModalEdit); break;
    case KeyKind::CtrlW: LineEdit_DeleteWordBack(&t->ModalEdit); break;
    case KeyKind::CtrlU: LineEdit_KillToStart(&t->ModalEdit); break;
    case KeyKind::CtrlK: LineEdit_KillToEnd(&t->ModalEdit); break;
    case KeyKind::Escape: FinishModal(t, TuiModalResult::Cancelled); break;
    case KeyKind::CtrlC:
      if (t->OperationActive) t->OperationAbortFlag = 1;
      FinishModal(t, TuiModalResult::Cancelled);
      break;
    default: break;
  }
}

void UpdateMouseDrag(Tui* t) {
  if (t->DragDomain == TuiDragDomain::None && t->PressedEntryId == 0) return;
  if (t->MouseX != t->MouseDownX || t->MouseY != t->MouseDownY) {
    t->MouseDragMoved = true;
    t->PressedEntryId = 0;
  }
  if (!t->MouseDragMoved) return;
  if (t->DragDomain == TuiDragDomain::None) return;

  if (t->DragDomain == TuiDragDomain::Composer) {
    SetComposerMouseCursor(t, t->MouseX, t->MouseY, true);
    return;
  }

  t->Top = t->LastTop;
  t->Follow = false;
  if (!ValidateTranscriptSelection(t) || t->LastPaneH == 0) return;
  u16 screenRow =
      t->MouseY < t->LastPaneH ? t->MouseY : (u16)(t->LastPaneH - 1u);
  TranscriptMousePoint(t, t->MouseX, screenRow, &t->TranscriptFocusRow,
                       &t->TranscriptFocusCell);
  RefreshTranscriptSelectionHash(t);

  i32 direction = 0;
  if (t->MouseY == 0)
    direction = -1;
  else if (t->MouseY >= (u16)(t->LastPaneH - 1u))
    direction = 1;
  if (direction != t->SelectionEdgeDirection) t->NextSelectionScrollMs = 0;
  t->SelectionEdgeDirection = direction;
}

bool AdvanceSelectionTick(Tui* t, u64 nowMs) {
  if (t->DragDomain != TuiDragDomain::Transcript || t->SelectionEdgeDirection == 0 ||
      t->LastPaneH == 0 || !ValidateTranscriptSelection(t))
    return false;
  if (t->NextSelectionScrollMs != 0 && nowMs < t->NextSelectionScrollMs) return false;
  t->NextSelectionScrollMs = nowMs + 50;

  u32 oldTop = t->Top;
  ScrollBy(t, t->SelectionEdgeDirection);
  t->Follow = false;
  if (t->Top == oldTop) return false;

  u32 count = LineModel_Count(&t->Model);
  if (count == 0) return false;
  u32 screenRow = t->SelectionEdgeDirection < 0 ? 0 : t->LastPaneH - 1u;
  t->TranscriptFocusRow = t->Top + screenRow;
  if (t->TranscriptFocusRow >= count) t->TranscriptFocusRow = count - 1u;
  t->TranscriptFocusCell = TranscriptMouseCell(t, t->MouseX);
  RefreshTranscriptSelectionHash(t);
  return true;
}

void QueueClipboard(Tui* t, StrView text) {
  StrBuf_Clear(&t->ClipboardRaw);
  StrBuf_Append(&t->ClipboardRaw, text);
}

bool QueueTranscriptClipboard(Tui* t) {
  TranscriptSelection selection;
  if (!GetTranscriptSelection(t, &selection)) return false;
  u32 count = LineModel_Count(&t->Model);
  if (count == 0 || selection.FirstRow >= count) return false;
  if (selection.LastRow >= count) selection.LastRow = count - 1u;

  Arena scratch = ArenaCreate();
  StrBuf_Clear(&t->ClipboardRaw);
  for (u32 row = selection.FirstRow; row <= selection.LastRow; row++) {
    const SpanLine* line = LineModel_At(&t->Model, row);
    DisplayRowLayout layout = LineModel_LayoutDisplayRow(line, t->Model.Width);
    DisplayRowRange range;
    if (TranscriptRangeForRow(&selection, row, &layout, &range)) {
      StrView text = LineModel_ExtractDisplayRow(&scratch, &layout, range.Start, range.End);
      StrBuf_Append(&t->ClipboardRaw, text);
    }
    if (row != selection.LastRow) StrBuf_AppendChar(&t->ClipboardRaw, '\n');
  }
  ArenaRelease(&scratch);
  return true;
}

bool PrepareClipboardOutput(Tui* t) {
  if (t->ClipboardRaw.Length == 0) return false;
  StrBuf_Clear(&t->ClipboardOsc);
  Term_AppendOSC52(&t->ClipboardOsc, StrBuf_View(&t->ClipboardRaw));
  StrBuf_Clear(&t->ClipboardRaw);
  return true;
}

void WriteTermOutput(void*, const char* data, u32 n) { Term_Write(data, n); }

bool DrainShutdownOutput(Tui* t, ScreenWriter write, void* user) {
  Mutex_Lock(&t->Lock);
  if (!t->Stop) {
    Mutex_Unlock(&t->Lock);
    return false;
  }
  bool writeClipboard = PrepareClipboardOutput(t);
  StrView clipboard = StrBuf_View(&t->ClipboardOsc);
  Mutex_Unlock(&t->Lock);
  if (writeClipboard && write) write(user, clipboard.Data, clipboard.Length);
  return true;
}

void HandleKey(Tui* t, const Key& k) {
  ValidateTranscriptSelection(t);
  if (k.Kind == KeyKind::Escape) {
    if (t->AgentRunning && !t->AbortFlag) {
      u64 now = MonotonicMillis();
      bool doubleEscape =
          t->EscapeArmed && now - t->LastEscapeMs <= kDoubleEscapeMs;
      t->LastEscapeMs = now;
      t->EscapeArmed = !doubleEscape;
      if (doubleEscape) t->AbortFlag = 1;
    } else {
      t->EscapeArmed = false;
      t->LastEscapeMs = 0;
    }
  } else if (k.Kind != KeyKind::None) {
    t->EscapeArmed = false;
    t->LastEscapeMs = 0;
  }

  if (k.Kind == KeyKind::CtrlD) {
    RequestStopLocked(t);
    return;
  }

  if (t->ModalResult == TuiModalResult::Pending) {
    if (t->ModalKind == TuiModalKind::Select)
      HandleSelectKey(t, k);
    else if (t->ModalKind == TuiModalKind::Text)
      HandleTextKey(t, k);
    return;
  }

  StrView commandQuery = {};
  if (CommandPaletteQuery(t, &commandQuery)) {
    u32 matchCount = CommandMatchCount(t, commandQuery);
    if (t->CommandPaletteSelected >= matchCount) t->CommandPaletteSelected = 0;
    switch (k.Kind) {
      case KeyKind::Up:
        if (t->CommandPaletteSelected > 0) t->CommandPaletteSelected--;
        return;
      case KeyKind::Down:
        if (t->CommandPaletteSelected + 1 < matchCount) t->CommandPaletteSelected++;
        return;
      case KeyKind::Enter:
        SubmitPaletteCommand(t, commandQuery);
        return;
      case KeyKind::Escape:
        t->CommandPaletteDismissed = true;
        return;
      default: break;
    }
  }

  switch (k.Kind) {
    case KeyKind::Char:
      if (!k.Alt && k.Codepoint >= 0x20) {
        FocusComposer(t);
        LineEdit_InsertChar(&t->Edit, k.Codepoint);
        ResetCommandPalette(t);
      }
      break;
    case KeyKind::Enter:
      FocusComposer(t);
      DoSubmit(t);
      break;
    case KeyKind::Newline:
      FocusComposer(t);
      LineEdit_Newline(&t->Edit);
      ResetCommandPalette(t);
      break;
    case KeyKind::Backspace:
      FocusComposer(t);
      LineEdit_Backspace(&t->Edit);
      ResetCommandPalette(t);
      break;
    case KeyKind::Delete:
      FocusComposer(t);
      LineEdit_Delete(&t->Edit);
      ResetCommandPalette(t);
      break;
    case KeyKind::Left:
      FocusComposer(t);
      LineEdit_Left(&t->Edit, k.Shift);
      break;
    case KeyKind::Right:
      FocusComposer(t);
      LineEdit_Right(&t->Edit, k.Shift);
      break;
    case KeyKind::Up:
      FocusComposer(t);
      if (k.Shift)
        LineEdit_Up(&t->Edit, true);
      else if (!LineEdit_Up(&t->Edit))
        LineEdit_HistoryPrev(&t->Edit);
      ResetCommandPalette(t);
      break;
    case KeyKind::Down:
      FocusComposer(t);
      if (k.Shift)
        LineEdit_Down(&t->Edit, true);
      else if (!LineEdit_Down(&t->Edit))
        LineEdit_HistoryNext(&t->Edit);
      ResetCommandPalette(t);
      break;
    case KeyKind::Home:
      FocusComposer(t);
      LineEdit_Home(&t->Edit, k.Shift);
      break;
    case KeyKind::End:
      FocusComposer(t);
      LineEdit_End(&t->Edit, k.Shift);
      break;
    case KeyKind::WordLeft:
      FocusComposer(t);
      LineEdit_WordLeft(&t->Edit, k.Shift);
      break;
    case KeyKind::WordRight:
      FocusComposer(t);
      LineEdit_WordRight(&t->Edit, k.Shift);
      break;
    case KeyKind::CtrlW:
      FocusComposer(t);
      LineEdit_DeleteWordBack(&t->Edit);
      ResetCommandPalette(t);
      break;
    case KeyKind::CtrlU:
      FocusComposer(t);
      LineEdit_KillToStart(&t->Edit);
      ResetCommandPalette(t);
      break;
    case KeyKind::CtrlK:
      FocusComposer(t);
      LineEdit_KillToEnd(&t->Edit);
      ResetCommandPalette(t);
      break;
    case KeyKind::CtrlL: t->Scr.ForceRepaint = true; break;
    case KeyKind::CtrlO: {
      ViewportAnchor anchor = CaptureViewportAnchor(t);
      if (LineModel_ToggleAllTools(&t->Model)) {
        RestoreViewportAnchor(t, anchor);
        ClearTranscriptSelection(t);
        t->ScrollbarDragging = false;
      }
      break;
    }
    case KeyKind::ScrollUp: ScrollBy(t, -kWheelStep); break;
    case KeyKind::ScrollDown: ScrollBy(t, kWheelStep); break;
    case KeyKind::MouseMove:
      if (t->ScrollbarDragging) {
        DragScrollbar(t, k);
        break;
      }
      UpdateMouse(t, k);
      UpdateMouseDrag(t);
      break;
    case KeyKind::MouseLeftDown: {
      if (IsScrollbarHit(t, k)) {
        BeginScrollbarDrag(t, k);
        break;
      }
      UpdateMouse(t, k);
      t->MouseDragMoved = false;
      t->MouseDownX = t->MouseX;
      t->MouseDownY = t->MouseY;
      if (HitComposer(t, t->MouseX, t->MouseY)) {
        ClearTranscriptSelection(t);
        SetComposerMouseCursor(t, t->MouseX, t->MouseY);
        t->DragDomain = TuiDragDomain::Composer;
        t->PressedEntryId = 0;
      } else if (TranscriptMousePoint(
                     t, t->MouseX, t->MouseY, &t->TranscriptAnchorRow,
                     &t->TranscriptAnchorCell)) {
        LineEdit_ClearSelection(&t->Edit);
        ClearTranscriptSelection(t);
        t->Top = t->LastTop;
        t->TranscriptFocusRow = t->TranscriptAnchorRow;
        t->TranscriptFocusCell = t->TranscriptAnchorCell;
        t->TranscriptSelectionRevision = LineModel_DisplayRevision(&t->Model);
        RefreshTranscriptSelectionHash(t);
        t->DragDomain = TuiDragDomain::Transcript;
        t->PressedEntryId = t->HoveredEntryId;
      } else {
        LineEdit_ClearSelection(&t->Edit);
        ClearTranscriptSelection(t);
        t->DragDomain = TuiDragDomain::None;
        t->PressedEntryId = 0;
      }
      break;
    }
    case KeyKind::MouseLeftUp: {
      if (t->ScrollbarDragging) {
        DragScrollbar(t, k);
        t->ScrollbarDragging = false;
        break;
      }
      UpdateMouse(t, k);
      UpdateMouseDrag(t);
      TuiDragDomain dragDomain = t->DragDomain;
      u64 pressed = t->PressedEntryId;
      t->PressedEntryId = 0;
      bool activate =
          !t->MouseDragMoved && pressed != 0 && pressed == t->HoveredEntryId;
      t->DragDomain = TuiDragDomain::None;
      t->SelectionEdgeDirection = 0;
      t->NextSelectionScrollMs = 0;
      if (activate) {
        ViewportAnchor anchor = CaptureViewportAnchor(t);
        if (LineModel_ToggleEntry(&t->Model, pressed)) RestoreViewportAnchor(t, anchor);
        ClearTranscriptSelection(t);
      } else if (dragDomain == TuiDragDomain::Transcript &&
                 (!t->MouseDragMoved || !TranscriptPointsDiffer(t))) {
        ClearTranscriptSelection(t);
      }
      break;
    }
    case KeyKind::CtrlC: {
      bool copied = false;
      if (LineEdit_HasSelection(&t->Edit)) {
        QueueClipboard(t, LineEdit_SelectedText(&t->Edit));
        copied = true;
      } else {
        copied = QueueTranscriptClipboard(t);
      }
      if (copied) break;
      if (t->OperationActive)
        t->OperationAbortFlag = 1;
      else if (t->AgentRunning)
        t->AbortFlag = 1;
      else if (!LineEdit_Empty(&t->Edit)) {
        LineEdit_Clear(&t->Edit);
        ResetCommandPalette(t);
      } else {
        RequestStopLocked(t);
      }
      break;
    }
    case KeyKind::PasteStart:
      FocusComposer(t);
      break;
    case KeyKind::PasteEnd: break;
    default: break;
  }
}

// ---- threads ----
void InputLoop(void* arg) {
  Tui* t = (Tui*)arg;
  char buf[1024];
  for (;;) {
    Mutex_Lock(&t->Lock);
    bool stop = t->Stop;
    Mutex_Unlock(&t->Lock);
    if (stop) break;
    if (!Term_WaitInput(&t->Trm, 50)) continue;

    u32 n = Term_ReadInput(&t->Trm, buf, sizeof(buf));
    if (n == 0) {
      Mutex_Lock(&t->Lock);
      if (t->Stop) {
        Mutex_Unlock(&t->Lock);
        break;
      }
      RequestStopLocked(t);
      Mutex_Unlock(&t->Lock);
      break;
    }
    Mutex_Lock(&t->Lock);
    stop = t->Stop;
    Mutex_Unlock(&t->Lock);
    if (stop) break;
    Tui_FeedInput(t, buf, n);
  }
}

// Coalesce bursty stream deltas into frames at up to ~60fps.
const u32 kMinFrameMs = 16;

void RenderLoop(void* arg) {
  Tui* t = (Tui*)arg;
  for (;;) {
    if (DrainShutdownOutput(t, WriteTermOutput, nullptr)) break;
    Mutex_Lock(&t->Lock);
    if (t->Stop) {
      Mutex_Unlock(&t->Lock);
      continue;
    }
    Compose(t);
    t->InputDirty = false;
    bool writeClipboard = PrepareClipboardOutput(t);
    Mutex_Unlock(&t->Lock);
    Screen_Flush(&t->Scr);
    if (writeClipboard) Term_Write(t->ClipboardOsc.Data, t->ClipboardOsc.Length);
    u64 lastFrame = MonotonicMillis();

    Mutex_Lock(&t->Lock);
    bool edgeDragging = t->DragDomain == TuiDragDomain::Transcript &&
                        t->SelectionEdgeDirection != 0;
    u32 interval = edgeDragging
                       ? kMinFrameMs
                       : (Tui_HasPendingDisplay(t)
                              ? kMinFrameMs
                              : ((t->State != TuiState::Idle ||
                                  t->OperationActive)
                                     ? 60
                                     : 200));
    if (!t->Dirty && !t->Stop) Cond_WaitTimeout(&t->RenderCv, &t->Lock, interval);
    t->Dirty = false;
    while (!t->Stop) {
      if (t->InputDirty) break;
      u64 elapsed = MonotonicMillis() - lastFrame;
      if (elapsed >= kMinFrameMs) break;
      Cond_WaitTimeout(&t->RenderCv, &t->Lock, (u32)(kMinFrameMs - elapsed));
    }
    Mutex_Unlock(&t->Lock);
  }
}

}  // namespace

void Tui_ComposeFrame(Tui* t, u16 cols, u16 rows) {
  ComposeAtSize(t, cols, rows, MonotonicMillis());
}

bool Tui_AdvanceStream(Tui* t, u64 nowMs) {
  bool changed = PumpDisplay(t, nowMs);
  ValidateTranscriptSelection(t);
  return changed;
}

bool Tui_HasPendingDisplay(const Tui* t) {
  return StreamReveal_HasPending(&t->Reveal) || HasQueuedDisplayEvents(t);
}

void Tui_TestCompose(Tui* t, u16 cols, u16 rows, u64 nowMs) {
  ComposeAtSize(t, cols, rows, nowMs ? nowMs : MonotonicMillis());
}

bool Tui_TestDrainShutdown(Tui* t, ScreenWriter write, void* user) {
  return DrainShutdownOutput(t, write, user);
}

bool Tui_Start(Tui* t, Arena* mem, TuiOpts opts) {
  if (!Term_IsInteractive()) return false;
  Tui_InitModel(t, mem, opts);
  if (!Term_Enter(&t->Trm)) {
    Tui_Stop(t);
    return false;
  }

  u16 cols = 80, rows = 24;
  Term_Size(&t->Trm, &cols, &rows);
  t->Scr.Write = [](void* u, const char* d, u32 n) { (void)u; Term_Write(d, n); };
  Screen_Resize(&t->Scr, cols, rows);
  LineModel_SetWidth(&t->Model, cols);

  // Avoid scheduler latency on the startup-critical frame.
  Compose(t);
  Screen_Flush(&t->Scr);

  if (!Thread_Start(&t->InputThread, InputLoop, t)) {
    Tui_Stop(t);
    return false;
  }
  t->InputThreadUp = true;
  if (!Thread_Start(&t->RenderThread, RenderLoop, t)) {
    Tui_Stop(t);
    return false;
  }
  t->RenderThreadUp = true;
  return true;
}

static StrView CopyStartupText(char* dst, u32 cap, StrView text) {
  u32 n = text.Length < cap - 1 ? text.Length : cap - 1;
  if (n) memcpy(dst, text.Data, n);
  dst[n] = 0;
  return StrView{dst, n};
}

void Tui_SetStartupContext(Tui* t, StrView cwd, StrView branch) {
  Mutex_Lock(&t->Lock);
  t->Cwd = CopyStartupText(t->CwdStorage, (u32)sizeof(t->CwdStorage), cwd);
  t->Branch =
      CopyStartupText(t->BranchStorage, (u32)sizeof(t->BranchStorage), branch);
  Wake(t);
  Mutex_Unlock(&t->Lock);
}

void Tui_LoadStartupContext(Tui* t) {
  Arena scratch = ArenaCreate((usize)4 << 20);
  StrView raw = Sys_CurrentDir(&scratch);
  StrView cwd = Sys_AbbreviatePath(&scratch, raw, Sys_HomeDir(&scratch));
  StrView branch = Sys_GitBranch(&scratch, raw);
  Tui_SetStartupContext(t, cwd, branch);
  ArenaRelease(&scratch);
}

void Tui_InitModel(Tui* t, Arena* mem, TuiOpts opts) {
  memset(t, 0, sizeof(*t));
  t->Mem = mem;
  g_color = opts.Color;
  LineModel_SetColorEnabled(opts.Color);
  t->ModelName = StrView_Dup(mem, opts.ModelName);
  if (opts.Commands && opts.CommandCount) {
    t->Commands = ArenaPushArray(mem, TuiCommandSpec, opts.CommandCount);
    t->CommandCount = opts.CommandCount;
    for (u32 i = 0; i < opts.CommandCount; i++) {
      t->Commands[i].Name = StrView_Dup(mem, opts.Commands[i].Name);
      t->Commands[i].Alias = StrView_Dup(mem, opts.Commands[i].Alias);
      t->Commands[i].Description = StrView_Dup(mem, opts.Commands[i].Description);
    }
  }
  t->ScreenMem = ArenaCreate();
  t->ScreenUp = true;
  Screen_Init(&t->Scr, &t->ScreenMem, nullptr, nullptr);
  t->SessionMem = ArenaCreate();
  LineModel_Init(&t->Model, &t->SessionMem, 80, 5000);
  LineEdit_Init(&t->Edit, &t->SessionMem);
  LineEdit_Init(&t->ModalEdit, &t->SessionMem);
  KeyDecoder_Init(&t->Dec);
  StreamReveal_Init(&t->Reveal, &t->SessionMem);
  t->DisplayEvents = Array_Make<TuiDisplayEvent>(&t->SessionMem, 16);
  t->ClipboardRaw = StrBuf_Make(mem, 256);
  t->ClipboardOsc = StrBuf_Make(mem, 384);
  Mutex_Init(&t->Lock);
  Cond_Init(&t->SubmitCv);
  Cond_Init(&t->ModalCv);
  Cond_Init(&t->RenderCv);
  t->SyncUp = true;
  t->SessionUp = true;
  t->Follow = true;
  t->State = TuiState::Idle;
}

bool Tui_ClearSession(Tui* t) {
  Mutex_Lock(&t->Lock);
  if (t->AgentRunning || (t->Ag && t->Ag->Streaming) || t->Stop || t->Quit) {
    Mutex_Unlock(&t->Lock);
    return false;
  }

  bool cancelModal = t->ModalResult == TuiModalResult::Pending;
  u16 width = t->Model.Width;
  u32 cap = t->Model.Cap;
  Arena nextSession = ArenaCreate();
  TuiSubmission preserved[64] = {};
  u32 preservedCount = t->QCount;
  for (u32 i = 0; i < preservedCount; i++) {
    const TuiSubmission& queued = t->Queue[(t->QHead + i) % 64];
    preserved[i].Kind = queued.Kind;
    preserved[i].Text = StrView_Dup(&nextSession, queued.Text);
  }

  LineModel_Release(&t->Model);
  ArenaRelease(&t->SessionMem);
  t->SessionMem = nextSession;
  LineModel_Init(&t->Model, &t->SessionMem, width, cap);
  LineEdit_Init(&t->Edit, &t->SessionMem);
  LineEdit_Init(&t->ModalEdit, &t->SessionMem);
  KeyDecoder_Init(&t->Dec);
  StreamReveal_Init(&t->Reveal, &t->SessionMem);
  t->DisplayEvents = Array_Make<TuiDisplayEvent>(&t->SessionMem, 16);
  t->DisplayHead = 0;

  memset(t->Queue, 0, sizeof(t->Queue));
  for (u32 i = 0; i < preservedCount; i++) t->Queue[i] = preserved[i];
  t->QHead = 0;
  t->QTail = preservedCount % 64;
  t->QCount = preservedCount;
  t->AgentRunning = false;
  t->AbortFlag = 0;
  t->LastEscapeMs = 0;
  t->EscapeArmed = false;
  t->State = TuiState::Idle;
  t->StateTool = {};
  t->TurnStartMs = 0;
  t->Use = {};
  t->Spinner = 0;
  t->QueuedCount = 0;
  t->CommandPaletteSelected = 0;
  t->CommandPaletteFirst = 0;
  t->CommandPaletteDismissed = false;
  t->OperationActive = false;
  t->OperationLabel = {};
  t->OperationStatus = {};
  t->OperationAbortFlag = 0;
  t->ModalKind = TuiModalKind::None;
  t->ModalResult = TuiModalResult::None;
  t->ModalTitle = {};
  t->ModalPrompt = {};
  t->ModalOptions = nullptr;
  t->ModalOptionCount = 0;
  t->ModalSelected = 0;
  t->ModalFirst = 0;
  t->ModalTextResult = {};
  t->Follow = true;
  t->Top = 0;
  t->LastTop = 0;
  t->LastPaneH = 0;
  ResetScrollbar(t);
  t->HoveredEntryId = 0;
  t->PressedEntryId = 0;
  t->MouseX = 0;
  t->MouseY = 0;
  t->MouseDownX = 0;
  t->MouseDownY = 0;
  t->HasMouse = false;
  t->MouseDragMoved = false;
  t->DragDomain = TuiDragDomain::None;
  t->TranscriptAnchorRow = 0;
  t->TranscriptFocusRow = 0;
  t->TranscriptAnchorCell = 0;
  t->TranscriptFocusCell = 0;
  t->TranscriptSelectionRevision = 0;
  t->TranscriptSelectionHash = 0;
  t->SelectionEdgeDirection = 0;
  t->NextSelectionScrollMs = 0;
  t->LastCols = 0;
  t->LastInputTop = 0;
  t->LastInputH = 0;
  t->LastInputFirstRow = 0;

  if (cancelModal) Cond_Broadcast(&t->ModalCv);
  Wake(t);
  Mutex_Unlock(&t->Lock);
  return true;
}

void Tui_Stop(Tui* t) {
  if (t->SyncUp) {
    Mutex_Lock(&t->Lock);
    RequestStopLocked(t);
    Mutex_Unlock(&t->Lock);
  }
  if (t->InputThreadUp) Term_WakeInput(&t->Trm);
  if (t->RenderThreadUp) {
    Thread_Join(&t->RenderThread);
    t->RenderThreadUp = false;
  }
  if (t->InputThreadUp) {
    Thread_Join(&t->InputThread);
    t->InputThreadUp = false;
  }
  Term_Exit(&t->Trm);
  if (t->ScreenUp) {
    ArenaRelease(&t->ScreenMem);
    t->ScreenUp = false;
  }
  if (t->SessionUp) {
    LineModel_Release(&t->Model);
    ArenaRelease(&t->SessionMem);
    t->SessionUp = false;
  }
  if (t->SyncUp) {
    Cond_Destroy(&t->RenderCv);
    Cond_Destroy(&t->ModalCv);
    Cond_Destroy(&t->SubmitCv);
    Mutex_Destroy(&t->Lock);
    t->SyncUp = false;
  }
}

bool Tui_WaitForSubmit(Tui* t, TuiSubmission* out) {
  Mutex_Lock(&t->Lock);
  for (;;) {
    if (t->Quit || t->Stop) {
      Mutex_Unlock(&t->Lock);
      return false;
    }
    if (QPop(t, out)) {
      Mutex_Unlock(&t->Lock);
      return true;
    }
    Cond_Wait(&t->SubmitCv, &t->Lock);
  }
}

bool Tui_AcceptPrompt(Tui* t, StrView text) {
  Mutex_Lock(&t->Lock);
  if (t->Stop || t->Quit) {
    Mutex_Unlock(&t->Lock);
    return false;
  }
  t->AgentRunning = true;
  QueueUserDisplayLocked(t, text);
  Wake(t);
  Mutex_Unlock(&t->Lock);
  return true;
}

void Tui_CommandNotice(Tui* t, StrView text) {
  Mutex_Lock(&t->Lock);
  TuiDisplayEvent* display = QueueDisplayEvent(t, TuiDisplayEventKind::Notice);
  SetDisplayPayload(t, display, text);
  t->Follow = true;
  Wake(t);
  Mutex_Unlock(&t->Lock);
}

void Tui_CommandError(Tui* t, StrView text) {
  Mutex_Lock(&t->Lock);
  TuiDisplayEvent* display = QueueDisplayEvent(t, TuiDisplayEventKind::Error);
  SetDisplayPayload(t, display, text);
  t->Follow = true;
  Wake(t);
  Mutex_Unlock(&t->Lock);
}

void Tui_SetModelName(Tui* t, StrView modelName) {
  Mutex_Lock(&t->Lock);
  t->ModelName = StrView_Dup(t->Mem, modelName);
  Wake(t);
  Mutex_Unlock(&t->Lock);
}

void Tui_BeginOperation(Tui* t, StrView label) {
  Mutex_Lock(&t->Lock);
  if (t->Stop || t->Quit) {
    t->OperationActive = false;
    t->OperationLabel = {};
    t->OperationStatus = {};
    t->OperationAbortFlag = 1;
    Mutex_Unlock(&t->Lock);
    return;
  }
  t->OperationActive = true;
  t->OperationLabel = StrView_Dup(&t->SessionMem, label);
  t->OperationStatus = {};
  t->OperationAbortFlag = 0;
  t->Spinner = 0;
  Wake(t);
  Mutex_Unlock(&t->Lock);
}

void Tui_SetOperationStatus(Tui* t, StrView status) {
  Mutex_Lock(&t->Lock);
  if (t->OperationActive) t->OperationStatus = StrView_Dup(&t->SessionMem, status);
  Wake(t);
  Mutex_Unlock(&t->Lock);
}

bool Tui_EndOperation(Tui* t) {
  Mutex_Lock(&t->Lock);
  bool cancelled = t->OperationAbortFlag != 0 || t->Stop || t->Quit;
  t->OperationActive = false;
  t->OperationLabel = {};
  t->OperationStatus = {};
  Wake(t);
  Mutex_Unlock(&t->Lock);
  return cancelled;
}

volatile i32* Tui_OperationAbort(Tui* t) { return &t->OperationAbortFlag; }

bool Tui_BeginSelection(Tui* t, StrView title, const StrView* options, u32 optionCount,
                        u32 initialSelection) {
  if (!options || optionCount == 0) return false;
  Mutex_Lock(&t->Lock);
  if (t->Stop || t->Quit || t->ModalResult == TuiModalResult::Pending) {
    Mutex_Unlock(&t->Lock);
    return false;
  }
  t->ModalOptions = ArenaPushArray(&t->SessionMem, StrView, optionCount);
  for (u32 i = 0; i < optionCount; i++)
    t->ModalOptions[i] = StrView_Dup(&t->SessionMem, options[i]);
  t->ModalOptionCount = optionCount;
  t->ModalSelected = initialSelection < optionCount ? initialSelection : optionCount - 1;
  t->ModalFirst = 0;
  t->ModalTitle = StrView_Dup(&t->SessionMem, title);
  t->ModalPrompt = {};
  t->ModalTextResult = {};
  t->ModalKind = TuiModalKind::Select;
  t->ModalResult = TuiModalResult::Pending;
  Wake(t);
  Mutex_Unlock(&t->Lock);
  return true;
}

bool Tui_BeginTextEntry(Tui* t, StrView title, StrView prompt, StrView initial) {
  Mutex_Lock(&t->Lock);
  if (t->Stop || t->Quit || t->ModalResult == TuiModalResult::Pending) {
    Mutex_Unlock(&t->Lock);
    return false;
  }
  LineEdit_Clear(&t->ModalEdit);
  LineEdit_InsertText(&t->ModalEdit, initial);
  t->ModalTitle = StrView_Dup(&t->SessionMem, title);
  t->ModalPrompt = StrView_Dup(&t->SessionMem, prompt);
  t->ModalOptions = nullptr;
  t->ModalOptionCount = 0;
  t->ModalSelected = 0;
  t->ModalFirst = 0;
  t->ModalTextResult = {};
  t->ModalKind = TuiModalKind::Text;
  t->ModalResult = TuiModalResult::Pending;
  Wake(t);
  Mutex_Unlock(&t->Lock);
  return true;
}

TuiModalResult Tui_GetModalResult(Tui* t, u32* selected, StrView* text) {
  Mutex_Lock(&t->Lock);
  TuiModalResult result = t->ModalResult;
  if (selected) *selected = t->ModalSelected;
  if (text) *text = t->ModalTextResult;
  Mutex_Unlock(&t->Lock);
  return result;
}

TuiModalResult Tui_Select(Tui* t, StrView title, const StrView* options,
                         u32 optionCount, u32* selected,
                         u32 initialSelection) {
  if (!Tui_BeginSelection(t, title, options, optionCount, initialSelection))
    return TuiModalResult::Cancelled;
  Mutex_Lock(&t->Lock);
  while (t->ModalResult == TuiModalResult::Pending && !t->Stop)
    Cond_Wait(&t->ModalCv, &t->Lock);
  TuiModalResult result = t->ModalResult;
  if (selected) *selected = t->ModalSelected;
  Mutex_Unlock(&t->Lock);
  return result;
}

TuiModalResult Tui_ReadText(Tui* t, StrView title, StrView prompt,
                           StrView initial, StrView* out) {
  if (!Tui_BeginTextEntry(t, title, prompt, initial))
    return TuiModalResult::Cancelled;
  Mutex_Lock(&t->Lock);
  while (t->ModalResult == TuiModalResult::Pending && !t->Stop)
    Cond_Wait(&t->ModalCv, &t->Lock);
  TuiModalResult result = t->ModalResult;
  if (out)
    *out = result == TuiModalResult::Confirmed ? t->ModalTextResult : StrView{};
  Mutex_Unlock(&t->Lock);
  return result;
}

void Tui_OpenCommandPalette(Tui* t) {
  Mutex_Lock(&t->Lock);
  if (!t->Stop && !t->Quit) {
    t->ModalKind = TuiModalKind::None;
    t->ModalResult = TuiModalResult::None;
    LineEdit_Clear(&t->Edit);
    LineEdit_InsertText(&t->Edit, SVL("/"));
    ResetCommandPalette(t);
    Wake(t);
  }
  Mutex_Unlock(&t->Lock);
}

volatile i32* Tui_Abort(Tui* t) { return &t->AbortFlag; }

void Tui_FeedInput(Tui* t, const char* data, u32 n) {
  Mutex_Lock(&t->Lock);
  if (n == 1 && data[0] == '\x1b' && t->Dec.Len == 0) {
    Key escape = {};
    escape.Kind = KeyKind::Escape;
    HandleKey(t, escape);
    WakeInput(t);
    Mutex_Unlock(&t->Lock);
    return;
  }
  KeyDecoder_Push(&t->Dec, data, n);
  Key k;
  while (KeyDecoder_Next(&t->Dec, &k)) HandleKey(t, k);
  WakeInput(t);
  Mutex_Unlock(&t->Lock);
}

void Tui_AgentEvent(void* user, const AgentEvent& ev) {
  Tui* t = (Tui*)user;
  Mutex_Lock(&t->Lock);
  switch (ev.Kind) {
    case AgentEventKind::AgentStart:
      t->AgentRunning = true;
      t->LastEscapeMs = 0;
      t->EscapeArmed = false;
      if (!Tui_HasPendingDisplay(t)) t->State = TuiState::Thinking;
      t->TurnStartMs = NowMillis();
      t->Spinner = 0;
      break;
    case AgentEventKind::MessageStart:
      // The agent emits message_start for the assistant turn before any content
      // exists (ev.Msg is the not-yet-built message, often null), so there is
      // nothing to render yet. The assistant entry is begun lazily on its first
      // text delta below. This mirrors pi, where the assistant block stays empty
      // (invisible) until content streams, so tool-only turns show no header.
      break;
    case AgentEventKind::MessageUpdate:
      if (ev.Stream.Kind == StreamEventKind::TextDelta) QueueText(t, ev.Stream.Delta);
      break;
    case AgentEventKind::MessageEnd: {
      if (!ev.Msg || ev.Msg->Kind != Role::Assistant) break;
      TuiDisplayEvent* display = QueueDisplayEvent(t, TuiDisplayEventKind::MessageEnd);
      display->Stop = ev.Msg->Stop;
      display->Use = ev.Msg->Use;
      if (ev.Msg->ErrorMessage.Length) SetDisplayPayload(t, display, ev.Msg->ErrorMessage);
      break;
    }
    case AgentEventKind::ToolStart: {
      TuiDisplayEvent* display = QueueDisplayEvent(t, TuiDisplayEventKind::ToolStart);
      display->ToolCallId = StrView_Dup(&t->SessionMem, ev.ToolCallId);
      display->ToolName = StrView_Dup(&t->SessionMem, ev.ToolName);
      display->ToolArgsJson = StrView_Dup(&t->SessionMem, ev.ToolArgsJson);
      break;
    }
    case AgentEventKind::ToolEnd: {
      StrView preview = StrView{nullptr, 0};
      if (ev.Result && ev.Result->Content.Count > 0) preview = ev.Result->Content.Data[0].Text;
      TuiDisplayEvent* display = QueueDisplayEvent(t, TuiDisplayEventKind::ToolEnd);
      display->ToolCallId = StrView_Dup(&t->SessionMem, ev.ToolCallId);
      display->Payload = StrBuf_Make(&t->SessionMem, preview.Length + 1);
      Sanitize_Display(&display->Payload, preview);
      display->IsError = ev.IsError;
      break;
    }
    case AgentEventKind::AgentEnd:
      t->AgentRunning = false;
      t->LastEscapeMs = 0;
      t->EscapeArmed = false;
      QueueDisplayEvent(t, TuiDisplayEventKind::AgentEnd);
      break;
    case AgentEventKind::QueueUpdate:
      t->QueuedCount = ev.SteerCount + ev.FollowUpCount + ev.NextTurnCount;
      break;
    default: break;
  }
  Wake(t);
  Mutex_Unlock(&t->Lock);
}
