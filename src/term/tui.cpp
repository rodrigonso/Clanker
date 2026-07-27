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
void QPush(Tui* t, StrView line) {
  if (t->QCount >= 64) return;
  t->Queue[t->QTail] = line;
  t->QTail = (t->QTail + 1) % 64;
  t->QCount++;
}
bool QPop(Tui* t, StrView* out) {
  if (t->QCount == 0) return false;
  *out = t->Queue[t->QHead];
  t->QHead = (t->QHead + 1) % 64;
  t->QCount--;
  return true;
}

void Wake(Tui* t) {
  t->Dirty = true;
  Cond_Signal(&t->RenderCv);
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
  ev->Payload = StrBuf_Make(t->Mem, text.Length + 1);
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
    DisplayRowLayout layout = LineModel_LayoutDisplayRow(line, t->LastCols);
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
  if (t->State == TuiState::Idle) {
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
  Screen_PutText(&t->Scr, 0, statusY, StrView{left, ll}, st, rightX ? rightX : cols);
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
  u16 x = Screen_PutText(&t->Scr, 0, cwdY, t->Cwd, st, cols);
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

ViewportAnchor CaptureViewportAnchor(const Tui* t) {
  ViewportAnchor anchor = {};
  if (t->Follow) return anchor;

  u32 count = LineModel_Count(&t->Model);
  u32 maxTop = count > t->LastPaneH ? count - t->LastPaneH : 0;
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
  u32 maxTop = lineCount > t->LastPaneH ? lineCount - t->LastPaneH : 0;
  if (t->Top > maxTop) t->Top = maxTop;
}

void ApplyDisplayEvent(Tui* t, const TuiDisplayEvent& ev) {
  switch (ev.Kind) {
    case TuiDisplayEventKind::MessageEnd:
      if (t->Model.HasAssistant) LineModel_EndAssistant(&t->Model);
      t->Use = ev.Use;
      if (ev.Stop == StopReason::Error)
        LineModel_AddNotice(&t->Model, EntryKind::Error, DisplayPayload(ev));
      else if (ev.Stop == StopReason::Aborted)
        LineModel_AddNotice(&t->Model, EntryKind::Info, SVL("[interrupted]"));
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

void ComposeSized(Tui* t, u16 cols, u16 rows, u64 nowMs) {
  if (cols != t->Scr.Cols || rows != t->Scr.Rows) {
    ViewportAnchor anchor = CaptureViewportAnchor(t);
    ClearTranscriptSelection(t);
    Screen_Resize(&t->Scr, cols, rows);
    LineModel_SetWidth(&t->Model, cols);
    RestoreViewportAnchor(t, anchor);
  }
  t->LastCols = cols;
  if (cols < 6 || rows < 6) {
    t->LastPaneH = 0;
    t->LastInputH = 0;
    Screen_BeginFrame(&t->Scr);
    return;
  }

  PumpDisplay(t, nowMs);
  AdvanceSelectionTick(t, nowMs);
  if (t->State != TuiState::Idle) t->Spinner++;

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
  u16 cwdY = (u16)(topBorderY - 1);
  u16 paneH = cwdY;  // rows [0, cwdY)
  t->LastPaneH = paneH;
  t->LastInputTop = inputTop;
  t->LastInputH = inputH;

  Screen_BeginFrame(&t->Scr);

  // Transcript.
  u32 count = LineModel_Count(&t->Model);
  u32 top;
  if (t->Follow) {
    top = count > paneH ? count - paneH : 0;
  } else {
    u32 maxTop = count > paneH ? count - paneH : 0;
    top = t->Top > maxTop ? maxTop : t->Top;
  }
  t->LastTop = top;
  if (t->HasMouse && t->MouseY < paneH) {
    const SpanLine* hoverLine = LineModel_At(&t->Model, top + t->MouseY);
    t->HoveredEntryId = hoverLine && hoverLine->IsEntryStart ? hoverLine->EntryId : 0;
  } else {
    t->HoveredEntryId = 0;
  }
  TranscriptSelection selection;
  bool hasSelection = GetTranscriptSelection(t, &selection);

  for (u16 r = 0; r < paneH; r++) {
    u32 idx = top + r;
    if (idx >= count) break;
    const SpanLine* line = LineModel_At(&t->Model, idx);
    bool hovered =
        line->IsEntryStart && line->EntryId != 0 && line->EntryId == t->HoveredEntryId;
    RenderSpanLine(t, r, idx, line, cols, hovered,
                   hasSelection ? &selection : nullptr);
  }

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
}

void Compose(Tui* t) {
  u16 cols = 0, rows = 0;
  Term_Size(&t->Trm, &cols, &rows);
  ComposeSized(t, cols, rows, MonotonicMillis());
}

// ---- key handling (lock held) ----
void DoSubmit(Tui* t) {
  StrView line = LineEdit_Submit(&t->Edit);
  if (line.Length == 0) return;
  TuiDisplayEvent* display = QueueDisplayEvent(t, TuiDisplayEventKind::User);
  SetDisplayPayload(t, display, line);
  display->Timestamp = NowMillis();
  t->Follow = true;
  if (t->AgentRunning && t->Ag) {
    Agent_Steer(t->Ag, line);  // mid-turn submit -> steer the running agent
  } else {
    QPush(t, line);  // idle -> next prompt for Tui_WaitForSubmit
    Cond_Signal(&t->SubmitCv);
  }
}

const i32 kWheelStep = 3;  // transcript rows per mouse-wheel notch

u64 HitTestEntry(const Tui* t, u16 y) {
  if (y >= t->LastPaneH) return 0;
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
  return x < t->LastCols ? x : (u16)(t->LastCols - 1u);
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
      LineModel_LayoutDisplayRow(LineModel_At(&t->Model, *row), t->LastCols);
  *cell = layout.ContentEnd ? (u16)(layout.ContentEnd - 1u) : 0;
  return true;
}

void UpdateMouse(Tui* t, const Key& key) {
  t->MouseX = key.MouseX;
  t->MouseY = key.MouseY;
  t->HasMouse = true;
  t->HoveredEntryId = HitTestEntry(t, key.MouseY);
}

void ScrollBy(Tui* t, i32 delta) {
  u32 count = LineModel_Count(&t->Model);
  u32 maxTop = count > t->LastPaneH ? count - t->LastPaneH : 0;
  u32 cur = t->Follow ? maxTop : t->Top;
  i64 nv = (i64)cur + delta;
  if (nv < 0) nv = 0;
  if (nv > (i64)maxTop) nv = maxTop;
  t->Top = (u32)nv;
  t->Follow = (t->Top >= maxTop);
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
    DisplayRowLayout layout = LineModel_LayoutDisplayRow(line, t->LastCols);
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
  switch (k.Kind) {
    case KeyKind::Char:
      if (!k.Alt && k.Codepoint >= 0x20) {
        FocusComposer(t);
        LineEdit_InsertChar(&t->Edit, k.Codepoint);
      }
      break;
    case KeyKind::Enter:
      FocusComposer(t);
      DoSubmit(t);
      break;
    case KeyKind::Newline:
      FocusComposer(t);
      LineEdit_Newline(&t->Edit);
      break;
    case KeyKind::Backspace:
      FocusComposer(t);
      LineEdit_Backspace(&t->Edit);
      break;
    case KeyKind::Delete:
      FocusComposer(t);
      LineEdit_Delete(&t->Edit);
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
      break;
    case KeyKind::Down:
      FocusComposer(t);
      if (k.Shift)
        LineEdit_Down(&t->Edit, true);
      else if (!LineEdit_Down(&t->Edit))
        LineEdit_HistoryNext(&t->Edit);
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
      break;
    case KeyKind::CtrlU:
      FocusComposer(t);
      LineEdit_KillToStart(&t->Edit);
      break;
    case KeyKind::CtrlK:
      FocusComposer(t);
      LineEdit_KillToEnd(&t->Edit);
      break;
    case KeyKind::CtrlL: t->Scr.ForceRepaint = true; break;
    case KeyKind::CtrlO: {
      ViewportAnchor anchor = CaptureViewportAnchor(t);
      if (LineModel_ToggleAllTools(&t->Model)) {
        RestoreViewportAnchor(t, anchor);
        ClearTranscriptSelection(t);
      }
      break;
    }
    case KeyKind::ScrollUp: ScrollBy(t, -kWheelStep); break;
    case KeyKind::ScrollDown: ScrollBy(t, kWheelStep); break;
    case KeyKind::MouseMove:
      UpdateMouse(t, k);
      UpdateMouseDrag(t);
      break;
    case KeyKind::MouseLeftDown: {
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
      if (t->AgentRunning)
        t->AbortFlag = 1;
      else if (!LineEdit_Empty(&t->Edit)) {
        LineEdit_Clear(&t->Edit);
      } else {
        t->Quit = true;
        t->Stop = true;
        Cond_Signal(&t->SubmitCv);
      }
      break;
    }
    case KeyKind::PasteStart:
      FocusComposer(t);
      break;
    case KeyKind::PasteEnd: break;
    case KeyKind::CtrlD:
      t->AbortFlag = 1;
      t->Quit = true;
      t->Stop = true;
      Cond_Signal(&t->SubmitCv);
      break;
    default: break;
  }
}

// ---- threads ----
void InputLoop(void* arg) {
  Tui* t = (Tui*)arg;
  char buf[1024];
  for (;;) {
    u32 n = Term_ReadInput(&t->Trm, buf, sizeof(buf));
    if (n == 0) {
      Mutex_Lock(&t->Lock);
      t->Quit = true;
      t->Stop = true;
      Cond_Signal(&t->SubmitCv);
      Wake(t);
      Mutex_Unlock(&t->Lock);
      break;
    }
    Tui_FeedInput(t, buf, n);
    Mutex_Lock(&t->Lock);
    bool stop = t->Stop;
    Mutex_Unlock(&t->Lock);
    if (stop) break;
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
                              : ((t->State != TuiState::Idle) ? 60 : 200));
    if (!t->Dirty && !t->Stop) Cond_WaitTimeout(&t->RenderCv, &t->Lock, interval);
    t->Dirty = false;
    while (!t->Stop) {
      u64 elapsed = MonotonicMillis() - lastFrame;
      if (elapsed >= kMinFrameMs) break;
      Cond_WaitTimeout(&t->RenderCv, &t->Lock, (u32)(kMinFrameMs - elapsed));
    }
    Mutex_Unlock(&t->Lock);
  }
}

}  // namespace

bool Tui_AdvanceStream(Tui* t, u64 nowMs) {
  bool changed = PumpDisplay(t, nowMs);
  ValidateTranscriptSelection(t);
  return changed;
}

bool Tui_HasPendingDisplay(const Tui* t) {
  return StreamReveal_HasPending(&t->Reveal) || HasQueuedDisplayEvents(t);
}

void Tui_TestCompose(Tui* t, u16 cols, u16 rows, u64 nowMs) {
  if (!t->Scr.Mem) Screen_Init(&t->Scr, t->Mem, nullptr, nullptr);
  ComposeSized(t, cols, rows, nowMs ? nowMs : MonotonicMillis());
}

bool Tui_TestDrainShutdown(Tui* t, ScreenWriter write, void* user) {
  return DrainShutdownOutput(t, write, user);
}

bool Tui_Start(Tui* t, Arena* mem, TuiOpts opts) {
  if (!Term_IsInteractive()) return false;
  Tui_InitModel(t, mem, opts);
  if (!Term_Enter(&t->Trm)) {
    LineModel_Release(&t->Model);
    return false;
  }

  u16 cols = 80, rows = 24;
  Term_Size(&t->Trm, &cols, &rows);
  Screen_Init(&t->Scr, mem, [](void* u, const char* d, u32 n) { (void)u; Term_Write(d, n); }, nullptr);
  Screen_Resize(&t->Scr, cols, rows);
  LineModel_SetWidth(&t->Model, cols);

  StrView raw = Sys_CurrentDir(mem);
  t->Cwd = Sys_AbbreviatePath(mem, raw, Sys_HomeDir(mem));
  t->Branch = Sys_GitBranch(mem, raw);

  Thread_Start(&t->RenderThread, RenderLoop, t);
  Thread_Start(&t->InputThread, InputLoop, t);
  t->ThreadsUp = true;
  return true;
}

void Tui_InitModel(Tui* t, Arena* mem, TuiOpts opts) {
  memset(t, 0, sizeof(*t));
  t->Mem = mem;
  g_color = opts.Color;
  LineModel_SetColorEnabled(opts.Color);
  t->ModelName = opts.ModelName;
  LineModel_Init(&t->Model, mem, 80, 5000);
  LineEdit_Init(&t->Edit, mem);
  KeyDecoder_Init(&t->Dec);
  StreamReveal_Init(&t->Reveal, mem);
  t->DisplayEvents = Array_Make<TuiDisplayEvent>(mem, 16);
  t->ClipboardRaw = StrBuf_Make(mem, 256);
  t->ClipboardOsc = StrBuf_Make(mem, 384);
  Mutex_Init(&t->Lock);
  Cond_Init(&t->SubmitCv);
  Cond_Init(&t->RenderCv);
  t->Follow = true;
  t->State = TuiState::Idle;
}

void Tui_Stop(Tui* t) {
  if (t->ThreadsUp) {
    Mutex_Lock(&t->Lock);
    t->Stop = true;
    Cond_Signal(&t->SubmitCv);
    Cond_Signal(&t->RenderCv);
    Mutex_Unlock(&t->Lock);
    Thread_Join(&t->RenderThread);
    Thread_Join(&t->InputThread);
    t->ThreadsUp = false;
    Term_Exit(&t->Trm);
  }
  LineModel_Release(&t->Model);
}

bool Tui_WaitForSubmit(Tui* t, StrView* out) {
  Mutex_Lock(&t->Lock);
  for (;;) {
    if (QPop(t, out)) {
      Mutex_Unlock(&t->Lock);
      return true;
    }
    if (t->Quit || t->Stop) {
      Mutex_Unlock(&t->Lock);
      return false;
    }
    Cond_Wait(&t->SubmitCv, &t->Lock);
  }
}

volatile i32* Tui_Abort(Tui* t) { return &t->AbortFlag; }

void Tui_FeedInput(Tui* t, const char* data, u32 n) {
  KeyDecoder_Push(&t->Dec, data, n);
  Mutex_Lock(&t->Lock);
  Key k;
  while (KeyDecoder_Next(&t->Dec, &k)) HandleKey(t, k);
  Wake(t);
  Mutex_Unlock(&t->Lock);
}

void Tui_AgentEvent(void* user, const AgentEvent& ev) {
  Tui* t = (Tui*)user;
  Mutex_Lock(&t->Lock);
  switch (ev.Kind) {
    case AgentEventKind::AgentStart:
      t->AgentRunning = true;
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
      display->ToolCallId = StrView_Dup(t->Mem, ev.ToolCallId);
      display->ToolName = StrView_Dup(t->Mem, ev.ToolName);
      display->ToolArgsJson = StrView_Dup(t->Mem, ev.ToolArgsJson);
      break;
    }
    case AgentEventKind::ToolEnd: {
      StrView preview = StrView{nullptr, 0};
      if (ev.Result && ev.Result->Content.Count > 0) preview = ev.Result->Content.Data[0].Text;
      TuiDisplayEvent* display = QueueDisplayEvent(t, TuiDisplayEventKind::ToolEnd);
      display->ToolCallId = StrView_Dup(t->Mem, ev.ToolCallId);
      display->Payload = StrBuf_Make(t->Mem, preview.Length + 1);
      Sanitize_Display(&display->Payload, preview);
      display->IsError = ev.IsError;
      break;
    }
    case AgentEventKind::AgentEnd:
      t->AgentRunning = false;
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
