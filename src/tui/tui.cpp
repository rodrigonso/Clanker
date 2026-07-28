#include "tui/tui.h"

#include <string.h>

#include "base/sysinfo.h"
#include "term/cell.h"
#include "term/sanitize.h"

namespace {

const char* kSpinner[10] = {"\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8",
                            "\xe2\xa0\xbc", "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7",
                            "\xe2\xa0\x87", "\xe2\xa0\x8f"};
constexpr u64 kDoubleEscapeMs = 500;

Style TuiSt(const Tui* t, u8 fg, u8 attr) {
  return Style{t->Color ? fg : COL_DEFAULT, COL_DEFAULT, attr};
}

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

const u8 SELECTION_BG = 238;

void ClearTranscriptSelection(Tui* t) {
  if (t->TranscriptSelectionRevision == 0 && !t->TranscriptDragging)
    return;
  t->TranscriptSelectionRevision = 0;
  t->TranscriptSelectionHash = 0;
  t->SelectionEdgeDirection = 0;
  t->NextSelectionScrollMs = 0;
  t->TranscriptDragging = false;
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

void FormatStatus(Tui* t, char* left, u32 leftCap, u32* ll, char* right,
                  u32 rightCap, u32* rl) {
  *ll = 0;
  if (t->OperationActive) {
    Put(left, leftCap, ll, kSpinner[t->Spinner % 10]);
    Put(left, leftCap, ll, " ");
    for (u32 i = 0; i < t->OperationLabel.Length && *ll < leftCap - 1; i++)
      left[(*ll)++] = t->OperationLabel.Data[i];
    if (t->OperationStatus.Length) {
      Put(left, leftCap, ll, ": ");
      for (u32 i = 0; i < t->OperationStatus.Length && *ll < leftCap - 1; i++)
        left[(*ll)++] = t->OperationStatus.Data[i];
    }
  } else if (t->State == TuiState::Idle) {
    Put(left, leftCap, ll, "ready");
  } else {
    Put(left, leftCap, ll, kSpinner[t->Spinner % 10]);
    Put(left, leftCap, ll, " ");
    if (t->State == TuiState::Tool) {
      Put(left, leftCap, ll, "running ");
      for (u32 i = 0; i < t->StateTool.Length && *ll < leftCap - 1; i++)
        left[(*ll)++] = t->StateTool.Data[i];
    } else if (t->State == TuiState::Streaming) {
      Put(left, leftCap, ll, "responding");
    } else {
      Put(left, leftCap, ll, "thinking");
    }
  }

  *rl = 0;
  for (u32 i = 0; i < t->ModelName.Length && *rl < rightCap - 1; i++)
    right[(*rl)++] = t->ModelName.Data[i];
  if (t->Use.Input || t->Use.Output) {
    Put(right, rightCap, rl, "  \xe2\x86\x91");
    PutScaled(right, rightCap, rl, t->Use.Input);
    Put(right, rightCap, rl, " \xe2\x86\x93");
    PutScaled(right, rightCap, rl, t->Use.Output);
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

struct CommandPaletteComponentData {
  Tui* Controller;
  StrView Query;
  u32 MatchCount;
  u16 TopBorderY;
  u16 Height;
  u16 Cols;
};

void PaintCommandPaletteNode(UiContext*, const UiNode* node,
                             UiCanvas* canvas) {
  CommandPaletteComponentData* data =
      (CommandPaletteComponentData*)node->User;
  Tui* t = data->Controller;
  if (data->Height < 4 || data->TopBorderY < data->Height) return;
  u16 y0 = (u16)(data->TopBorderY - data->Height);
  u16 y1 = (u16)(data->TopBorderY - 1);
  u16 panelW = data->Cols > 76 ? 72 : (u16)(data->Cols - 2);
  u16 x0 = 1;
  u16 x1 = (u16)(x0 + panelW - 1);
  Style panel = TuiSt(t, 252, ATTR_NONE);
  UiRect panelRect{x0, y0, (u16)(x1 - x0 + 1),
                   (u16)(y1 - y0 + 1)};
  UiCanvas_Fill(canvas, panelRect, panel);
  UiCanvas_DrawBorder(canvas, panelRect, panel);

  Style title = panel;
  title.Attr |= ATTR_BOLD;
  UiCanvas_PutTextClipped(canvas, (u16)(x0 + 2), (u16)(y0 + 1),
                          SVL("Commands"), title, x1);

  u32 visible = data->Height - 3;
  if (data->MatchCount == 0) {
    t->CommandPaletteSelected = 0;
    t->CommandPaletteFirst = 0;
    Style empty = panel;
    empty.Attr |= ATTR_DIM;
    UiCanvas_PutTextClipped(canvas, (u16)(x0 + 2), (u16)(y0 + 2),
                            SVL("No matching commands"), empty, x1);
    return;
  }

  if (t->CommandPaletteSelected >= data->MatchCount)
    t->CommandPaletteSelected = 0;
  if (t->CommandPaletteSelected < t->CommandPaletteFirst)
    t->CommandPaletteFirst = t->CommandPaletteSelected;
  if (t->CommandPaletteSelected >= t->CommandPaletteFirst + visible)
    t->CommandPaletteFirst = t->CommandPaletteSelected - visible + 1;
  u32 maxFirst =
      data->MatchCount > visible ? data->MatchCount - visible : 0;
  if (t->CommandPaletteFirst > maxFirst) t->CommandPaletteFirst = maxFirst;

  for (u32 row = 0; row < visible; row++) {
    u32 matchIndex = t->CommandPaletteFirst + row;
    const TuiCommandSpec* command =
        CommandMatchAt(t, data->Query, matchIndex);
    if (!command) break;
    u16 y = (u16)(y0 + 2 + row);
    bool selected = matchIndex == t->CommandPaletteSelected;
    Style option = panel;
    if (selected) {
      option.Attr |= ATTR_REVERSE;
      UiCanvas_Fill(canvas,
                    UiRect{(u16)(x0 + 1), y, (u16)(x1 - x0 - 1), 1},
                    option);
    }
    u16 x = UiCanvas_PutTextClipped(
        canvas, (u16)(x0 + 2), y,
        selected ? SVL("\xe2\x80\xba ") : SVL("  "), option, x1);
    Style name = option;
    name.Attr |= ATTR_BOLD;
    x = UiCanvas_PutTextClipped(canvas, x, y, SVL("/"), name, x1);
    x = UiCanvas_PutTextClipped(canvas, x, y, command->Name, name, x1);
    u16 descriptionX = (u16)(x0 + 18);
    if (descriptionX < x + 2) descriptionX = (u16)(x + 2);
    Style description = option;
    description.Attr |= ATTR_DIM;
    UiCanvas_PutTextClipped(canvas, descriptionX, y,
                            command->Description, description, x1);
  }
}

struct ModalComponentData {
  Tui* Controller;
  u16 Cols;
  u16 TopBorderY;
};

void PaintModalNode(UiContext*, const UiNode* node, UiCanvas* canvas) {
  ModalComponentData* data = (ModalComponentData*)node->User;
  Tui* t = data->Controller;
  if (t->ModalKind == TuiModalKind::None ||
      t->ModalResult != TuiModalResult::Pending || data->TopBorderY < 4)
    return;

  u16 panelW = data->Cols > 76 ? 72 : (u16)(data->Cols - 2);
  u16 desiredH =
      t->ModalKind == TuiModalKind::Select
          ? (u16)((t->ModalOptionCount < 12 ? t->ModalOptionCount : 12) + 3)
          : 6;
  u16 panelH = desiredH < data->TopBorderY ? desiredH : data->TopBorderY;
  if (panelH < 4) return;
  u16 x0 = 1;
  u16 y0 = (u16)(data->TopBorderY - panelH);
  u16 x1 = (u16)(x0 + panelW - 1);
  u16 y1 = (u16)(data->TopBorderY - 1);
  bool compactText = t->ModalKind == TuiModalKind::Text && panelH < 5;
  Style panel = TuiSt(t, 252, ATTR_NONE);
  UiRect panelRect{x0, y0, (u16)(x1 - x0 + 1),
                   (u16)(y1 - y0 + 1)};
  UiCanvas_Fill(canvas, panelRect, panel);
  UiCanvas_DrawBorder(canvas, panelRect, panel);

  Style path = panel;
  path.Attr |= ATTR_DIM;
  u16 titleX = UiCanvas_PutTextClipped(
      canvas, (u16)(x0 + 2), (u16)(y0 + 1), SVL("Commands  /  "),
      path, (u16)(x1 - 1));
  Style title = panel;
  title.Attr |= ATTR_BOLD;
  UiCanvas_PutTextClipped(
      canvas, titleX, (u16)(y0 + 1),
      compactText ? t->ModalPrompt : t->ModalTitle, title,
      (u16)(x1 - 1));

  if (t->ModalKind == TuiModalKind::Select) {
    u32 visible = panelH > 3 ? panelH - 3 : 1;
    if (t->ModalSelected < t->ModalFirst) t->ModalFirst = t->ModalSelected;
    if (t->ModalSelected >= t->ModalFirst + visible)
      t->ModalFirst = t->ModalSelected - visible + 1;
    u32 maxFirst =
        t->ModalOptionCount > visible ? t->ModalOptionCount - visible : 0;
    if (t->ModalFirst > maxFirst) t->ModalFirst = maxFirst;
    for (u32 row = 0; row < visible; row++) {
      u32 index = t->ModalFirst + row;
      if (index >= t->ModalOptionCount) break;
      u16 y = (u16)(y0 + 2 + row);
      bool selected = index == t->ModalSelected;
      Style option = panel;
      if (selected) option.Attr |= ATTR_REVERSE;
      if (selected)
        UiCanvas_Fill(
            canvas, UiRect{(u16)(x0 + 1), y, (u16)(x1 - x0 - 1), 1},
            option);
      UiCanvas_PutTextClipped(
          canvas, (u16)(x0 + 2), y,
          selected ? SVL("\xe2\x80\xba ") : SVL("  "), option,
          (u16)(x1 - 1));
      UiCanvas_PutTextClipped(canvas, (u16)(x0 + 4), y,
                              t->ModalOptions[index], option,
                              (u16)(x1 - 1));
    }
    UiCanvas_SetCursor(canvas, node->Id, false, 0, 0);
    return;
  }

  Style prompt = panel;
  prompt.Attr |= ATTR_DIM;
  if (!compactText)
    UiCanvas_PutTextClipped(canvas, (u16)(x0 + 2), (u16)(y0 + 2),
                            t->ModalPrompt, prompt, (u16)(x1 - 1));
  u16 editY = (u16)(y1 - 1);
  Style edit = panel;
  edit.Attr |= ATTR_REVERSE;
  UiCanvas_Fill(canvas,
                UiRect{(u16)(x0 + 2), editY, (u16)(x1 - x0 - 3), 1},
                edit);
  StrView text = LineEdit_Text(&t->ModalEdit);
  UiCanvas_PutTextClipped(canvas, (u16)(x0 + 3), editY, text, edit,
                          (u16)(x1 - 2));
  u32 cursorW =
      TextWidth(StrView{text.Data, LineEdit_Cursor(&t->ModalEdit)});
  u16 cursorX = (u16)(x0 + 3 + cursorW);
  if (cursorX >= x1 - 2) cursorX = (u16)(x1 - 3);
  UiCanvas_SetCursor(canvas, node->Id, true, cursorX, editY);
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

void DoSubmit(Tui* t);

void ComposerSubmit(void* user) { DoSubmit((Tui*)user); }

void ComposerChanged(void* user) { ResetCommandPalette((Tui*)user); }

void ComposerInteract(void* user) {
  Tui* t = (Tui*)user;
  FocusComposer(t);
  t->PressedEntryId = 0;
  t->ScrollbarDragging = false;
}

bool ComposerHistoryPrevious(void* user) {
  Tui* t = (Tui*)user;
  LineEdit_HistoryPrev(&t->Edit);
  return true;
}

bool ComposerHistoryNext(void* user) {
  Tui* t = (Tui*)user;
  LineEdit_HistoryNext(&t->Edit);
  return true;
}

struct TranscriptComponentData {
  Tui* Controller;
  TranscriptLayout Layout;
  u16 Height;
  bool HasSelection;
  TranscriptSelection Selection;
};

bool EventTranscriptNode(UiContext* ui, UiNode* node, const UiEvent& event);

void CancelTranscriptCapture(void* persistent) {
  Tui* t = (Tui*)persistent;
  if (!t) return;
  t->ScrollbarDragging = false;
  t->PressedEntryId = 0;
  t->TranscriptDragging = false;
  t->SelectionEdgeDirection = 0;
  t->NextSelectionScrollMs = 0;
}

Style SelectionTransform(Style style, void* user) {
  Tui* t = (Tui*)user;
  if (t->Color)
    style.Bg = SELECTION_BG;
  else if (style.Attr & ATTR_UNDERLINE)
    style.Attr |= ATTR_REVERSE;
  else
    style.Attr |= ATTR_UNDERLINE;
  return style;
}

Style TranscriptHoverStyle(const Tui* t, Style style) {
  if (t->Color)
    style.Bg = 235;
  else
    style.Attr |= ATTR_DIM;
  return style;
}

void PaintTranscriptNode(UiContext*, const UiNode* node, UiCanvas* canvas) {
  TranscriptComponentData* data = (TranscriptComponentData*)node->User;
  Tui* t = data->Controller;
  for (u16 r = 0; r < data->Height; r++) {
    u32 idx = data->Layout.Top + r;
    if (idx >= data->Layout.Count) break;
    const SpanLine* line = LineModel_At(&t->Model, idx);
    bool hovered =
        line->IsEntryStart && line->EntryId != 0 &&
        line->EntryId == t->HoveredEntryId;
    if (data->Layout.Cols && hovered)
      UiCanvas_Fill(
          canvas, UiRect{0, r, data->Layout.Cols, 1},
          TranscriptHoverStyle(t, StyleDefault()));
    DisplayRowLayout layout =
        LineModel_LayoutDisplayRow(line, data->Layout.Cols);
    DisplayRowIterator iterator = {};
    DisplayRowRun run;
    while (LineModel_NextDisplayRowRun(&layout, &iterator, &run)) {
      Style style =
          hovered ? TranscriptHoverStyle(t, run.St) : run.St;
      UiCanvas_PutTextClipped(
          canvas, run.Column, r, run.Text, style,
          (u16)(run.Column + run.CellWidth));
    }
    DisplayRowRange selected;
    if (data->HasSelection &&
        TranscriptRangeForRow(&data->Selection, idx, &layout, &selected))
      UiCanvas_TransformStyles(
          canvas, UiRect{selected.Start, r,
                         (u16)(selected.End - selected.Start), 1},
          SelectionTransform, t);
  }

  t->LastScrollbarThumbTop = 0;
  t->LastScrollbarThumbH = 0;
  if (!t->ScrollbarVisible || data->Height == 0 ||
      data->Layout.Count <= data->Height)
    return;
  ScrollbarGeometry geometry =
      CalcScrollbarGeometry(data->Layout.Count, data->Height, data->Layout.Top);
  t->LastScrollbarThumbTop = geometry.ThumbTop;
  t->LastScrollbarThumbH = geometry.ThumbH;
  u16 x = (u16)(t->Scr.Cols - 1);
  Style track =
      t->Color ? StyleMake(238, COL_DEFAULT, ATTR_NONE)
               : StyleMake(COL_DEFAULT, COL_DEFAULT, ATTR_DIM);
  Style thumb =
      t->Color ? StyleMake(250, COL_DEFAULT, ATTR_NONE)
               : StyleMake(COL_DEFAULT, COL_DEFAULT, ATTR_BOLD);
  for (u16 y = 0; y < data->Height; y++)
    UiCanvas_PutChar(canvas, x, y, 0x2595, track);
  for (u16 y = 0; y < geometry.ThumbH; y++)
    UiCanvas_PutChar(canvas, x, (u16)(geometry.ThumbTop + y), 0x2595,
                     thumb);
}

UiLayoutStyle Positioned(u16 x, u16 y, u16 width, u16 height) {
  UiLayoutStyle layout = {};
  layout.Width = Ui_Cells(width);
  layout.Height = Ui_Cells(height);
  layout.OffsetX = (i16)x;
  layout.OffsetY = (i16)y;
  return layout;
}

void BuildStatusComponents(Tui* t, u16 statusY, u16 cols) {
  char* left = (char*)Ui_FrameAlloc(&t->Ui, 256, alignof(char));
  char* right = (char*)Ui_FrameAlloc(&t->Ui, 256, alignof(char));
  u32 leftLength = 0;
  u32 rightLength = 0;
  FormatStatus(t, left, 256, &leftLength, right, 256, &rightLength);

  UiLayoutStyle row = Positioned(0, statusY, cols, 1);
  row.Padding.Left = 1;
  Ui_BeginRow(&t->Ui, Ui_Key(SVL("status")), row);
  Style style = TuiSt(t, 244, ATTR_NONE);
  UiTextProps leftProps = {};
  leftProps.Layout.Width = Ui_Fill();
  leftProps.Layout.Height = Ui_Fill();
  leftProps.StyleValue = style;
  Ui_Text(&t->Ui, Ui_Key(SVL("left")), StrView{left, leftLength}, leftProps);
  UiTextProps rightProps = {};
  rightProps.Layout.Width = Ui_Content();
  rightProps.Layout.Height = Ui_Fill();
  rightProps.StyleValue = style;
  Ui_Text(&t->Ui, Ui_Key(SVL("right")), StrView{right, rightLength},
          rightProps);
  Ui_EndContainer(&t->Ui);
}

void BuildCwdComponent(Tui* t, u16 cwdY, u16 cols) {
  if (t->Cwd.Length == 0 || cols <= 1) return;
  Style style = TuiSt(t, 244, ATTR_NONE);
  UiTextSpan spans[4];
  u32 count = 0;
  spans[count++] = UiTextSpan{t->Cwd, style};
  if (t->Branch.Length) {
    spans[count++] = UiTextSpan{SVL("  (\xe2\x8e\x87 "), style};
    spans[count++] = UiTextSpan{t->Branch, style};
    spans[count++] = UiTextSpan{SVL(")"), style};
  }
  UiTextProps props = {};
  props.Layout = Positioned(1, cwdY, (u16)(cols - 1), 1);
  props.StyleValue = style;
  Ui_StyledText(&t->Ui, Ui_Key(SVL("cwd")), spans, count, props);
}

void ComposeAtSize(Tui* t, u16 cols, u16 rows, u64 nowMs) {
  bool sizeChanged = cols != t->Scr.Cols || rows != t->Scr.Rows;
  if (sizeChanged) {
    ClearTranscriptSelection(t);
    Screen_Resize(&t->Scr, cols, rows);
  }
  if (cols < 6 || rows < 6) {
    ViewportAnchor anchor = CaptureViewportAnchor(t);
    ResetScrollbar(t);
    t->LastPaneH = 0;
    t->LastTop = 0;
    LineModel_SetWidth(&t->Model, cols);
    RestoreViewportAnchor(t, anchor);
    Ui_BeginFrame(&t->Ui, &t->Scr);
    Ui_EndFrame(&t->Ui);
    Ui_Render(&t->Ui, cols, rows);
    return;
  }

  PumpDisplay(t, nowMs);
  AdvanceSelectionTick(t, nowMs);
  if (t->State != TuiState::Idle || t->OperationActive) t->Spinner++;

  // Input box layout: left/right border column + one padding column each side.
  u16 textW = (u16)(cols > 4 ? cols - 4 : 1);
  LineEditLayout inputLayout;
  LineEdit_Layout(&t->Edit, textW, &inputLayout);
  u16 maxInputH = (u16)(rows / 2);
  if (maxInputH < 1) maxInputH = 1;
  if (maxInputH > 10) maxInputH = 10;
  u16 inputH =
      (u16)(inputLayout.Count < maxInputH ? inputLayout.Count : maxInputH);
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
  // Leave a blank row between the transcript and the sticky context line when
  // the terminal is tall enough to retain at least one transcript row.
  u16 paneH = cwdY > 1 ? (u16)(cwdY - 1) : cwdY;  // rows [0, paneH)
  TranscriptLayout transcript =
      LayoutTranscript(t, cols, paneH, sizeChanged || paneH != t->LastPaneH);

  // Transcript.
  if (t->HasMouse && t->MouseY < paneH &&
      (!t->ScrollbarVisible || t->MouseX != ScrollbarX(t))) {
    const SpanLine* hoverLine = LineModel_At(&t->Model, transcript.Top + t->MouseY);
    t->HoveredEntryId = hoverLine && hoverLine->IsEntryStart ? hoverLine->EntryId : 0;
  } else {
    t->HoveredEntryId = 0;
  }
  TranscriptSelection selection = {};
  bool hasSelection = GetTranscriptSelection(t, &selection);
  u32 firstRow = inputLayout.CursorRow >= inputH
                     ? inputLayout.CursorRow - inputH + 1
                     : 0;
  if (t->InputUi.Dragging) {
    u32 maxFirstRow =
        inputLayout.Count > inputH ? inputLayout.Count - inputH : 0;
    firstRow = t->InputUi.FirstRow < maxFirstRow
                   ? t->InputUi.FirstRow
                   : maxFirstRow;
  }

  Ui_BeginFrame(&t->Ui, &t->Scr);
  UiLayoutStyle root = {};
  root.Width = Ui_Fill();
  root.Height = Ui_Fill();
  Ui_BeginStack(&t->Ui, Ui_Key(SVL("root")), root);

  TranscriptComponentData* transcriptData =
      (TranscriptComponentData*)Ui_FrameAlloc(
          &t->Ui, sizeof(TranscriptComponentData),
          alignof(TranscriptComponentData));
  *transcriptData =
      TranscriptComponentData{t, transcript, paneH, hasSelection, selection};
  UiNodeDesc transcriptNode = {};
  transcriptNode.Kind = UiNodeKind::Custom;
  transcriptNode.Key = Ui_Key(SVL("transcript"));
  transcriptNode.Ops.Paint = PaintTranscriptNode;
  transcriptNode.Ops.Event = EventTranscriptNode;
  transcriptNode.Ops.Cancel = CancelTranscriptCapture;
  transcriptNode.User = transcriptData;
  transcriptNode.Persistent = t;
  transcriptNode.Layout = Positioned(0, 0, cols, paneH);
  Ui_Leaf(&t->Ui, transcriptNode);

  BuildCwdComponent(t, cwdY, cols);

  UiBorderProps inputBorder = {};
  inputBorder.Layout =
      Positioned(0, topBorderY, cols, (u16)(inputH + 2));
  inputBorder.StyleValue = TuiSt(t, 250, ATTR_NONE);
  Ui_BeginBorder(&t->Ui, Ui_Key(SVL("composer-border")), inputBorder);
  Ui_EndContainer(&t->Ui);

  t->InputUi.Edit = &t->Edit;
  t->InputUi.FirstRow = firstRow;
  UiTextInputProps inputProps = {};
  inputProps.Layout =
      Positioned(1, inputTop, (u16)(cols - 2), inputH);
  inputProps.State = &t->InputUi;
  inputProps.TextStyle = StyleDefault();
  inputProps.SelectionStyle =
      t->Color ? StyleMake(COL_DEFAULT, SELECTION_BG, ATTR_NONE)
               : StyleMake(COL_DEFAULT, COL_DEFAULT, ATTR_UNDERLINE);
  inputProps.WrapWidth = textW;
  inputProps.InsetLeft = 1;
  inputProps.InsetRight = 1;
  inputProps.OnSubmit = ComposerSubmit;
  inputProps.OnChange = ComposerChanged;
  inputProps.OnInteract = ComposerInteract;
  inputProps.OnUpBoundary = ComposerHistoryPrevious;
  inputProps.OnDownBoundary = ComposerHistoryNext;
  inputProps.User = t;
  Ui_TextInput(&t->Ui, Ui_Key(SVL("composer-input")), inputProps);

  BuildStatusComponents(t, statusY, cols);

  if (commandPalette && paletteH) {
    CommandPaletteComponentData* paletteData =
        (CommandPaletteComponentData*)Ui_FrameAlloc(
            &t->Ui, sizeof(CommandPaletteComponentData),
            alignof(CommandPaletteComponentData));
    *paletteData = CommandPaletteComponentData{
        t, commandQuery, commandMatches, topBorderY, paletteH, cols};
    UiNodeDesc paletteNode = {};
    paletteNode.Kind = UiNodeKind::Custom;
    paletteNode.Key = Ui_Key(SVL("command-palette"));
    paletteNode.Ops.Paint = PaintCommandPaletteNode;
    paletteNode.User = paletteData;
    paletteNode.Layout.Width = Ui_Fill();
    paletteNode.Layout.Height = Ui_Fill();
    Ui_Leaf(&t->Ui, paletteNode);
  }

  if (t->ModalKind != TuiModalKind::None &&
      t->ModalResult == TuiModalResult::Pending) {
    ModalComponentData* modalData =
        (ModalComponentData*)Ui_FrameAlloc(
            &t->Ui, sizeof(ModalComponentData), alignof(ModalComponentData));
    *modalData = ModalComponentData{t, cols, topBorderY};
    UiNodeDesc modalNode = {};
    modalNode.Kind = UiNodeKind::Custom;
    modalNode.Key = Ui_Key(SVL("modal"));
    modalNode.Ops.Paint = PaintModalNode;
    modalNode.User = modalData;
    modalNode.Layout.Width = Ui_Fill();
    modalNode.Layout.Height = Ui_Fill();
    Ui_Leaf(&t->Ui, modalNode);
  }

  Ui_EndContainer(&t->Ui);
  Ui_EndFrame(&t->Ui);
  Ui_Render(&t->Ui, cols, rows);
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

  if (t->AgentRunning && t->Harness) {
    QueueUserDisplayLocked(t, line);
    SessionResult queued = AgentHarness_QueueSteering(t->Harness, line);
    if (!SessionResult_IsOk(queued)) {
      TuiDisplayEvent* display =
          QueueDisplayEvent(t, TuiDisplayEventKind::Error);
      SetDisplayPayload(t, display, queued.Message);
    }
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
      t->ModalTextResult =
          StrView_Dup(&t->SessionMem, LineEdit_Text(&t->ModalEdit));
      FinishModal(t, TuiModalResult::Confirmed);
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
  if (!t->TranscriptDragging && t->PressedEntryId == 0) return;
  if (t->MouseX != t->MouseDownX || t->MouseY != t->MouseDownY) {
    t->MouseDragMoved = true;
    t->PressedEntryId = 0;
  }
  if (!t->MouseDragMoved) return;
  if (!t->TranscriptDragging) return;

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
  if (!t->TranscriptDragging || t->SelectionEdgeDirection == 0 ||
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

Key MouseKey(const UiEvent& event) {
  Key key = {};
  key.MouseX =
      event.X < 0 ? 0 : (event.X > 0xffff ? 0xffff : (u16)event.X);
  key.MouseY =
      event.Y < 0 ? 0 : (event.Y > 0xffff ? 0xffff : (u16)event.Y);
  return key;
}

bool EventTranscriptNode(UiContext* ui, UiNode* node,
                         const UiEvent& event) {
  TranscriptComponentData* data = (TranscriptComponentData*)node->User;
  Tui* t = data->Controller;
  if (event.Kind == UiEventKind::Wheel) {
    ScrollBy(t, event.Delta * kWheelStep);
    return true;
  }

  if (event.Kind != UiEventKind::MouseMove &&
      event.Kind != UiEventKind::MouseDown &&
      event.Kind != UiEventKind::MouseUp)
    return false;
  Key key = MouseKey(event);

  if (event.Kind == UiEventKind::MouseMove) {
    if (t->ScrollbarDragging) {
      DragScrollbar(t, key);
      return true;
    }
    UpdateMouse(t, key);
    UpdateMouseDrag(t);
    return true;
  }

  if (event.Kind == UiEventKind::MouseDown) {
    if (IsScrollbarHit(t, key)) {
      BeginScrollbarDrag(t, key);
      Ui_CapturePointer(ui, node->Id);
      return true;
    }
    UpdateMouse(t, key);
    t->MouseDragMoved = false;
    t->MouseDownX = t->MouseX;
    t->MouseDownY = t->MouseY;
    LineEdit_ClearSelection(&t->Edit);
    if (TranscriptMousePoint(
            t, t->MouseX, t->MouseY, &t->TranscriptAnchorRow,
            &t->TranscriptAnchorCell)) {
      ClearTranscriptSelection(t);
      t->Top = t->LastTop;
      t->TranscriptFocusRow = t->TranscriptAnchorRow;
      t->TranscriptFocusCell = t->TranscriptAnchorCell;
      t->TranscriptSelectionRevision =
          LineModel_DisplayRevision(&t->Model);
      RefreshTranscriptSelectionHash(t);
      t->TranscriptDragging = true;
      t->PressedEntryId = t->HoveredEntryId;
      Ui_CapturePointer(ui, node->Id);
    } else {
      ClearTranscriptSelection(t);
      t->TranscriptDragging = false;
      t->PressedEntryId = 0;
    }
    return true;
  }

  if (t->ScrollbarDragging) {
    DragScrollbar(t, key);
    t->ScrollbarDragging = false;
    Ui_ReleasePointer(ui, node->Id);
    return true;
  }
  UpdateMouse(t, key);
  UpdateMouseDrag(t);
  bool transcriptDragging = t->TranscriptDragging;
  u64 pressed = t->PressedEntryId;
  t->PressedEntryId = 0;
  bool activate =
      !t->MouseDragMoved && pressed != 0 && pressed == t->HoveredEntryId;
  t->TranscriptDragging = false;
  t->SelectionEdgeDirection = 0;
  t->NextSelectionScrollMs = 0;
  if (activate) {
    ViewportAnchor anchor = CaptureViewportAnchor(t);
    if (LineModel_ToggleEntry(&t->Model, pressed))
      RestoreViewportAnchor(t, anchor);
    ClearTranscriptSelection(t);
  } else if (transcriptDragging &&
             (!t->MouseDragMoved || !TranscriptPointsDiffer(t))) {
    ClearTranscriptSelection(t);
  }
  Ui_ReleasePointer(ui, node->Id);
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

bool IsComposerKey(KeyKind kind) {
  switch (kind) {
    case KeyKind::Char:
    case KeyKind::Enter:
    case KeyKind::Newline:
    case KeyKind::Backspace:
    case KeyKind::Delete:
    case KeyKind::Tab:
    case KeyKind::Left:
    case KeyKind::Right:
    case KeyKind::Up:
    case KeyKind::Down:
    case KeyKind::Home:
    case KeyKind::End:
    case KeyKind::WordLeft:
    case KeyKind::WordRight:
    case KeyKind::CtrlU:
    case KeyKind::CtrlK:
    case KeyKind::CtrlW:
      return true;
    default:
      return false;
  }
}

bool DispatchComposerKey(Tui* t, const Key& key) {
  if (t->Ui.Nodes.Count == 0 || !IsComposerKey(key.Kind)) return false;
  return Ui_Dispatch(&t->Ui, Ui_EventFromKey(key));
}

bool DispatchPointerKey(Tui* t, const Key& key) {
  bool hasFrame = t->Ui.Nodes.Count != 0;
  if (key.Kind == KeyKind::ScrollUp || key.Kind == KeyKind::ScrollDown) {
    if (hasFrame && Ui_Dispatch(&t->Ui, Ui_EventFromKey(key))) return true;
    ScrollBy(t, key.Kind == KeyKind::ScrollUp ? -kWheelStep : kWheelStep);
    return true;
  }
  if (key.Kind != KeyKind::MouseMove &&
      key.Kind != KeyKind::MouseLeftDown &&
      key.Kind != KeyKind::MouseLeftUp)
    return false;

  UpdateMousePosition(t, key);
  if (key.Kind == KeyKind::MouseLeftUp) {
    if (hasFrame)
      Ui_Dispatch(&t->Ui, Ui_EventFromKey(key));
    return true;
  }
  if (hasFrame && Ui_Dispatch(&t->Ui, Ui_EventFromKey(key))) return true;
  if (key.Kind == KeyKind::MouseMove) {
    t->HoveredEntryId = 0;
    return true;
  }

  Ui_CancelPointer(&t->Ui);
  LineEdit_ClearSelection(&t->Edit);
  ClearTranscriptSelection(t);
  CancelTranscriptCapture(t);
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
      if (doubleEscape) {
        if (t->Harness) {
          SessionResult cancelled =
              AgentHarness_RequestCancel(t->Harness);
          if (!SessionResult_IsOk(cancelled)) t->AbortFlag = 1;
        } else {
          t->AbortFlag = 1;
        }
      }
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

  if (DispatchPointerKey(t, k)) {
    return;
  }
  if (DispatchComposerKey(t, k)) {
    return;
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
      else if (t->AgentRunning) {
        if (t->Harness) {
          SessionResult cancelled =
              AgentHarness_RequestCancel(t->Harness);
          if (!SessionResult_IsOk(cancelled)) t->AbortFlag = 1;
        } else {
          t->AbortFlag = 1;
        }
      }
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
    bool edgeDragging =
        t->TranscriptDragging && t->SelectionEdgeDirection != 0;
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

bool Tui_AdvanceStream(Tui* t, u64 nowMs) {
  bool changed = PumpDisplay(t, nowMs);
  ValidateTranscriptSelection(t);
  return changed;
}

bool Tui_HasPendingDisplay(const Tui* t) {
  return StreamReveal_HasPending(&t->Reveal) || HasQueuedDisplayEvents(t);
}

void Tui_ComposeFrame(Tui* t, u16 cols, u16 rows, u64 nowMs) {
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
  t->Color = opts.Color;
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
  Ui_Init(&t->Ui);
  t->UiUp = true;
  t->SessionMem = ArenaCreate();
  LineModel_Init(&t->Model, &t->SessionMem, 80, 5000, opts.Color);
  LineEdit_Init(&t->Edit, &t->SessionMem);
  t->InputUi.Edit = &t->Edit;
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
  if (t->AgentRunning || (t->Harness && t->Harness->Running) || t->Stop ||
      t->Quit) {
    Mutex_Unlock(&t->Lock);
    return false;
  }

  bool cancelModal = t->ModalResult == TuiModalResult::Pending;
  Ui_CancelPointer(&t->Ui);
  t->Ui.Active = 0;
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
  LineModel_Init(&t->Model, &t->SessionMem, width, cap, t->Color);
  LineEdit_Init(&t->Edit, &t->SessionMem);
  t->InputUi = UiTextInputState{&t->Edit, 0, false};
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
  t->TranscriptDragging = false;
  t->TranscriptAnchorRow = 0;
  t->TranscriptFocusRow = 0;
  t->TranscriptAnchorCell = 0;
  t->TranscriptFocusCell = 0;
  t->TranscriptSelectionRevision = 0;
  t->TranscriptSelectionHash = 0;
  t->SelectionEdgeDirection = 0;
  t->NextSelectionScrollMs = 0;

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
  if (t->UiUp) {
    Ui_Release(&t->Ui);
    t->UiUp = false;
  }
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
  Ui_CancelPointer(&t->Ui);
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
  Ui_CancelPointer(&t->Ui);
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

static void HydrateTranscriptLocked(Tui* t, const Message* messages,
                                    u32 count) {
  for (u32 i = 0; i < count; i++) {
    const Message* message = &messages[i];
    if (message->Kind == Role::User) {
      if (t->Model.HasAssistant) LineModel_EndAssistant(&t->Model);
      StrBuf text = StrBuf_Make(&t->SessionMem, 128);
      for (u32 c = 0; c < message->Content.Count; c++) {
        const Content* content = &message->Content.Data[c];
        if (content->Kind != ContentKind::Text) continue;
        if (text.Length) StrBuf_AppendChar(&text, '\n');
        StrBuf_Append(&text, content->Text);
      }
      LineModel_AddUser(&t->Model, StrBuf_View(&text), message->Timestamp);
    } else if (message->Kind == Role::Assistant) {
      bool hasText = false;
      for (u32 c = 0; c < message->Content.Count; c++)
        if (message->Content.Data[c].Kind == ContentKind::Text &&
            message->Content.Data[c].Text.Length)
          hasText = true;
      if (hasText) {
        LineModel_BeginAssistant(&t->Model);
        for (u32 c = 0; c < message->Content.Count; c++)
          if (message->Content.Data[c].Kind == ContentKind::Text)
            LineModel_AppendAssistant(&t->Model,
                                      message->Content.Data[c].Text);
        LineModel_EndAssistant(&t->Model);
      }
      for (u32 c = 0; c < message->Content.Count; c++) {
        const Content* content = &message->Content.Data[c];
        if (content->Kind == ContentKind::ToolCall)
          LineModel_AddTool(&t->Model, content->ToolId, content->ToolName,
                            content->ToolArgsJson);
      }
      if (message->Stop == StopReason::Error)
        LineModel_AddNotice(&t->Model, EntryKind::Error,
                            message->ErrorMessage);
      else if (message->Stop == StopReason::Aborted)
        LineModel_AddNotice(&t->Model, EntryKind::Info,
                            SVL("Interrupted by user."));
    } else if (message->Kind == Role::ToolResult) {
      StrBuf text = StrBuf_Make(&t->SessionMem, 128);
      for (u32 c = 0; c < message->Content.Count; c++) {
        const Content* content = &message->Content.Data[c];
        if (content->Kind != ContentKind::Text) continue;
        if (text.Length) StrBuf_AppendChar(&text, '\n');
        StrBuf_Append(&text, content->Text);
      }
      StrView result = StrBuf_View(&text);
      if (!LineModel_FinishTool(&t->Model, message->ToolCallId, result,
                                Truncate(result, 1500), message->IsError))
        LineModel_AddNotice(&t->Model, EntryKind::Error,
                            SVL("tool result did not match a pending call"));
    }
  }
}

void Tui_HydrateTranscript(Tui* t, const Message* messages, u32 count) {
  if (!t || (!messages && count)) return;
  Mutex_Lock(&t->Lock);
  HydrateTranscriptLocked(t, messages, count);
  Wake(t);
  Mutex_Unlock(&t->Lock);
}

void Tui_ReplaceTranscript(Tui* t, const Message* messages, u32 count) {
  if (!t || (!messages && count)) return;
  Mutex_Lock(&t->Lock);
  u16 width = t->Model.Width;
  u32 cap = t->Model.Cap;
  LineModel_Release(&t->Model);
  LineModel_Init(&t->Model, &t->SessionMem, width, cap, t->Color);
  StreamReveal_Init(&t->Reveal, &t->SessionMem);
  t->DisplayEvents = Array_Make<TuiDisplayEvent>(&t->SessionMem, 16);
  t->DisplayHead = 0;
  t->Use = {};
  t->StateTool = {};
  t->State = t->AgentRunning ? TuiState::Thinking : TuiState::Idle;
  HydrateTranscriptLocked(t, messages, count);
  Wake(t);
  Mutex_Unlock(&t->Lock);
}

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
