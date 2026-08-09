#include "tui/internal.h"

#include <string.h>

#include "base/sysinfo.h"
#include "base/thread.h"
#include "term/cell.h"
#include "term/sanitize.h"

namespace TuiInternal {



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

void ResetToolUpdates(Tui* t) {
  ArenaReset(&t->ToolUpdateMem);
  memset(t->ToolUpdates, 0, sizeof(t->ToolUpdates));
}

TuiToolUpdateSlot* FindToolUpdate(Tui* t, StrView toolCallId) {
  for (u32 i = 0; i < TUI_TOOL_UPDATE_SLOT_COUNT; i++) {
    TuiToolUpdateSlot* slot = &t->ToolUpdates[i];
    if ((slot->Active || slot->Queued) &&
        StrView_Eq(slot->ToolCallId, toolCallId))
      return slot;
  }
  return nullptr;
}

TuiToolUpdateSlot* AcquireToolUpdate(Tui* t, StrView toolCallId,
                                     u16* outIndex) {
  if (TuiToolUpdateSlot* existing = FindToolUpdate(t, toolCallId)) {
    *outIndex = (u16)(existing - t->ToolUpdates);
    return existing;
  }

  bool anyInUse = false;
  for (u32 i = 0; i < TUI_TOOL_UPDATE_SLOT_COUNT; i++)
    if (t->ToolUpdates[i].Active || t->ToolUpdates[i].Queued)
      anyInUse = true;
  if (!anyInUse) ResetToolUpdates(t);

  for (u32 i = 0; i < TUI_TOOL_UPDATE_SLOT_COUNT; i++) {
    TuiToolUpdateSlot* slot = &t->ToolUpdates[i];
    if (slot->Active || slot->Queued) continue;
    slot->ToolCallId = StrView_Dup(&t->SessionMem, toolCallId);
    if (!slot->Payload.Data)
      slot->Payload = StrBuf_Make(&t->ToolUpdateMem, 1501);
    slot->Active = true;
    *outIndex = (u16)i;
    return slot;
  }
  return nullptr;
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


// Number of Unicode scalars in a UTF-8 string, so the copy confirmation counts
// characters rather than bytes.
u32 Utf8Count(StrView s) {
  u32 offset = 0;
  u32 count = 0;
  while (offset < s.Length) {
    Utf8Decode(s, &offset);
    count++;
  }
  return count;
}

void ClearCopyNotice(Tui* t) {
  t->CopyNoticeActive = false;
  t->CopyNoticeChars = 0;
  t->CopyNoticeExpiryMs = 0;
}

// Show a copy confirmation on the status row. The deadline stays unset until a
// frame stamps it, so expiry is driven by the compose clock.
void ArmCopyNotice(Tui* t, StrView text) {
  t->CopyNoticeActive = true;
  t->CopyNoticeChars = Utf8Count(text);
  t->CopyNoticeExpiryMs = 0;
}

// Stamp a freshly armed notice with its deadline, then retire it once the
// compose clock passes that deadline.
void AdvanceCopyNotice(Tui* t, u64 nowMs) {
  if (!t->CopyNoticeActive) return;
  if (t->CopyNoticeExpiryMs == 0) {
    t->CopyNoticeExpiryMs = nowMs + kCopyNoticeMs;
    return;
  }
  if (nowMs >= t->CopyNoticeExpiryMs) ClearCopyNotice(t);
}


u32 MaxTop(u32 count, u16 paneH) { return count > paneH ? count - paneH : 0; }

u16 ScrollbarX(const Tui* t) { return t->Scr.Cols ? (u16)(t->Scr.Cols - 1) : 0; }

void ResetScrollbar(Tui* t) {
  t->ScrollbarVisible = false;
  t->ScrollbarDragging = false;
  t->LastScrollbarThumbTop = 0;
  t->LastScrollbarThumbH = 0;
  t->ScrollbarDragOffset = 0;
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
      t->Sess.StateTool = ev.ToolName;
      break;
    case TuiDisplayEventKind::ToolUpdate:
      if (ev.ToolUpdateSlot < TUI_TOOL_UPDATE_SLOT_COUNT) {
        TuiToolUpdateSlot* slot = &t->ToolUpdates[ev.ToolUpdateSlot];
        LineModel_UpdateTool(&t->Model, ev.ToolCallId,
                             Truncate(StrBuf_View(&slot->Payload), 1500));
        slot->Queued = false;
      }
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
  bool toolStartedThisFrame = false;

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
    TuiDisplayEvent* ev = &t->DisplayEvents.Data[t->DisplayHead];
    if (toolStartedThisFrame && ev->Kind != TuiDisplayEventKind::ToolStart) break;
    t->DisplayHead++;
    if (ev->Kind == TuiDisplayEventKind::Text) {
      StreamReveal_Adopt(&t->Reveal, ev->Payload);
      t->State = TuiState::Streaming;
      continue;
    }
    ApplyDisplayEvent(t, *ev);
    if (ev->Kind == TuiDisplayEventKind::ToolStart)
      toolStartedThisFrame = true;
    changed = true;
  }

  CompactDisplayEvents(t);
  RestoreViewportAnchor(t, anchor);
  return changed;
}


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

void ComposeAtSize(Tui* t, u16 cols, u16 rows, u64 nowMs) {
  bool sizeChanged = cols != t->Scr.Cols || rows != t->Scr.Rows;
  if (sizeChanged) {
    ClearTranscriptSelection(t);
    Screen_Resize(&t->Scr, cols, rows);
  }
  AdvanceCopyNotice(t, nowMs);
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
  if (t->State != TuiState::Idle || t->OperationActive) {
    if (t->SpinnerStartMs == 0 || nowMs < t->SpinnerStartMs)
      t->SpinnerStartMs = nowMs;
    t->Spinner = (u32)((nowMs - t->SpinnerStartMs) / kSpinnerFrameMs);
  } else {
    t->Spinner = 0;
    t->SpinnerStartMs = 0;
  }

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

  BuildDebugOverlayComponent(t, cols, rows, nowMs);

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

  if (t->AgentRunning && t->Agent.Steer) {
    QueueUserDisplayLocked(t, line);
    StrView error = {};
    if (!t->Agent.Steer(t->Agent.User, line, &error)) {
      TuiDisplayEvent* display =
          QueueDisplayEvent(t, TuiDisplayEventKind::Error);
      SetDisplayPayload(t, display, error);
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
  // Any deliberate keystroke retires the copy confirmation early. Idle pointer
  // motion does not, so a drag-select followed by Ctrl+C keeps it visible. A
  // copying Ctrl+C re-arms below, after this clear.
  if (k.Kind != KeyKind::None && k.Kind != KeyKind::MouseMove)
    ClearCopyNotice(t);
  if (k.Kind == KeyKind::Escape) {
    bool canArm =
        t->ModalResult != TuiModalResult::Pending &&
        ((t->AgentRunning && !Atomic_LoadI32(&t->AbortFlag)) ||
         (!t->AgentRunning && !t->OperationActive));
    if (canArm) {
      u64 now = MonotonicMillis();
      bool doubleEscape =
          t->EscapeArmed && now - t->LastEscapeMs <= kDoubleEscapeMs;
      t->LastEscapeMs = now;
      t->EscapeArmed = !doubleEscape;
      if (doubleEscape) {
        if (t->AgentRunning) {
          if (!t->Agent.RequestCancel ||
              !t->Agent.RequestCancel(t->Agent.User))
            Atomic_StoreI32(&t->AbortFlag, 1);
        } else {
          FocusComposer(t);
          LineEdit_Clear(&t->Edit);
          ResetCommandPalette(t);
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
      if (copied) {
        ArmCopyNotice(t, StrBuf_View(&t->ClipboardRaw));
        break;
      }
      if (t->OperationActive)
        Atomic_StoreI32(&t->OperationAbortFlag, 1);
      else if (t->AgentRunning) {
        if (!t->Agent.RequestCancel ||
            !t->Agent.RequestCancel(t->Agent.User))
          Atomic_StoreI32(&t->AbortFlag, 1);
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
    u64 frameEndUs = MonotonicMicros();
    if (writeClipboard) Term_Write(t->ClipboardOsc.Data, t->ClipboardOsc.Length);
    u64 lastFrame = MonotonicMillis();

    Mutex_Lock(&t->Lock);
    TuiPerf_FrameRendered(&t->Perf, frameEndUs);
    bool edgeDragging =
        t->TranscriptDragging && t->SelectionEdgeDirection != 0;
    u32 interval = edgeDragging
                       ? kMinFrameMs
                       : (Tui_HasPendingDisplay(t)
                              ? kMinFrameMs
                              : ((t->State != TuiState::Idle ||
                                  t->OperationActive || t->CopyNoticeActive)
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

}  // namespace TuiInternal

using namespace TuiInternal;

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
  t->Profile = opts.Profile;
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
  t->ToolUpdateMem = ArenaCreate((usize)4 << 20);
  t->ToolUpdateUp = true;
  LineModel_Init(&t->Model, &t->SessionMem, 80, 5000, opts.Color);
  LineEdit_Init(&t->Edit, &t->SessionMem);
  t->InputUi.Edit = &t->Edit;
  LineEdit_Init(&t->ModalEdit, &t->SessionMem);
  KeyDecoder_Init(&t->Dec);
  StreamReveal_Init(&t->Reveal, &t->SessionMem);
  t->DisplayEvents = Array_Make<TuiDisplayEvent>(&t->SessionMem, 16);
  ResetToolUpdates(t);
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
  Mutex_Lock(&t->Lock);  if (t->AgentRunning || (t->Agent.IsBusy && t->Agent.IsBusy(t->Agent.User)) || t->Stop ||
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
  ResetToolUpdates(t);

  memset(t->Queue, 0, sizeof(t->Queue));
  for (u32 i = 0; i < preservedCount; i++) t->Queue[i] = preserved[i];
  t->QHead = 0;
  t->QTail = preservedCount % 64;
  t->QCount = preservedCount;
  t->AgentRunning = false;
  Atomic_StoreI32(&t->AbortFlag, 0);
  t->LastEscapeMs = 0;
  t->EscapeArmed = false;
  ClearCopyNotice(t);
  t->State = TuiState::Idle;
  // This path releases SessionMem too, so it clears the scoped strings the same
  // exhaustive way the rebuild does.
  ClearSessionScopedStringsLocked(t);
  t->Use = {};
  t->Spinner = 0;
  t->SpinnerStartMs = 0;
  t->QueuedCount = 0;
  t->CommandPaletteSelected = 0;
  t->CommandPaletteFirst = 0;
  t->CommandPaletteDismissed = false;
  t->OperationActive = false;
  Atomic_StoreI32(&t->OperationAbortFlag, 0);
  t->ModalKind = TuiModalKind::None;
  t->ModalResult = TuiModalResult::None;
  t->ModalSelected = 0;
  t->ModalFirst = 0;
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
  if (t->ToolUpdateUp) {
    ArenaRelease(&t->ToolUpdateMem);
    t->ToolUpdateUp = false;
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

bool Tui_UpdateDebugOverlay(Tui* t, TuiDebugOverlayAction action) {
  Mutex_Lock(&t->Lock);
  bool wasVisible = t->Perf.Visible;
  t->Perf.Visible = action == TuiDebugOverlayAction::Toggle
                        ? !t->Perf.Visible
                        : action == TuiDebugOverlayAction::Show;
  bool visible = t->Perf.Visible;
  if (!wasVisible && visible) t->Perf.LastRssSampleMs = 0;
  Wake(t);
  Mutex_Unlock(&t->Lock);
  return visible;
}

void Tui_SetModelProfile(Tui* t, StrView modelName,
                         ModelRequestProfile profile) {
  Mutex_Lock(&t->Lock);
  t->ModelName = StrView_Dup(t->Mem, modelName);
  t->Profile = profile;
  Wake(t);
  Mutex_Unlock(&t->Lock);
}

void Tui_BeginOperation(Tui* t, StrView label) {
  Mutex_Lock(&t->Lock);
  if (t->Stop || t->Quit) {
    t->OperationActive = false;
    t->Sess.OperationLabel = {};
    t->Sess.OperationStatus = {};
    Atomic_StoreI32(&t->OperationAbortFlag, 1);
    Mutex_Unlock(&t->Lock);
    return;
  }
  t->OperationActive = true;
  t->Sess.OperationLabel = StrView_Dup(&t->SessionMem, label);
  t->Sess.OperationStatus = {};
  Atomic_StoreI32(&t->OperationAbortFlag, 0);
  t->Spinner = 0;
  t->SpinnerStartMs = 0;
  Wake(t);
  Mutex_Unlock(&t->Lock);
}

void Tui_SetOperationStatus(Tui* t, StrView status) {
  Mutex_Lock(&t->Lock);
  if (t->OperationActive) t->Sess.OperationStatus = StrView_Dup(&t->SessionMem, status);
  Wake(t);
  Mutex_Unlock(&t->Lock);
}

bool Tui_EndOperation(Tui* t) {
  Mutex_Lock(&t->Lock);
  bool cancelled = Atomic_LoadI32(&t->OperationAbortFlag) != 0 || t->Stop || t->Quit;
  t->OperationActive = false;
  t->Sess.OperationLabel = {};
  t->Sess.OperationStatus = {};
  Wake(t);
  Mutex_Unlock(&t->Lock);
  return cancelled;
}

volatile i32* Tui_OperationAbort(Tui* t) { return &t->OperationAbortFlag; }

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

// ---- progress reporting -------------------------------------------------
// One call per thing the driver observed. Each takes the lock, records display
// state, and wakes the render thread; none of them knows what produced it.

void Tui_TurnStarted(Tui* t) {
  Mutex_Lock(&t->Lock);
  t->AgentRunning = true;
  t->Sess.RetryStatus = {};
  t->LastEscapeMs = 0;
  t->EscapeArmed = false;
  if (!Tui_HasPendingDisplay(t)) t->State = TuiState::Thinking;
  t->Spinner = 0;
  t->SpinnerStartMs = 0;
  TuiPerf_TurnStarted(&t->Perf, MonotonicMicros());
  Wake(t);
  Mutex_Unlock(&t->Lock);
}

void Tui_AppendAssistantText(Tui* t, StrView text) {
  Mutex_Lock(&t->Lock);
  QueueText(t, text);
  // Transcript reveal is rate-limited, so measure TTFT at arrival.
  TuiPerf_AssistantDelta(&t->Perf, MonotonicMicros());
  Wake(t);
  Mutex_Unlock(&t->Lock);
}

void Tui_AssistantMessageEnded(Tui* t, StopReason stop, Usage use,
                               StrView errorMessage) {
  Mutex_Lock(&t->Lock);
  TuiDisplayEvent* display = QueueDisplayEvent(t, TuiDisplayEventKind::MessageEnd);
  display->Stop = stop;
  display->Use = use;
  if (errorMessage.Length) SetDisplayPayload(t, display, errorMessage);
  TuiPerf_TurnMessageEnded(&t->Perf, use, MonotonicMicros());
  Wake(t);
  Mutex_Unlock(&t->Lock);
}

void Tui_ToolStarted(Tui* t, StrView callId, StrView name, StrView argumentsJson) {
  Mutex_Lock(&t->Lock);
  TuiDisplayEvent* display = QueueDisplayEvent(t, TuiDisplayEventKind::ToolStart);
  display->ToolCallId = StrView_Dup(&t->SessionMem, callId);
  display->ToolName = StrView_Dup(&t->SessionMem, name);
  display->ToolArgsJson = StrView_Dup(&t->SessionMem, argumentsJson);
  TuiPerf_ToolStarted(&t->Perf, name, MonotonicMicros());
  Wake(t);
  Mutex_Unlock(&t->Lock);
}

void Tui_ToolProgress(Tui* t, StrView callId, StrView preview) {
  Mutex_Lock(&t->Lock);
  u16 slotIndex = 0;
  TuiToolUpdateSlot* slot = AcquireToolUpdate(t, callId, &slotIndex);
  if (slot) {
    StrBuf_Clear(&slot->Payload);
    Sanitize_Display(&slot->Payload, Truncate(preview, 1500));
    // Only queue once per slot: a fast tool would otherwise fill the display
    // queue with updates that all render the same latest preview.
    if (!slot->Queued) {
      TuiDisplayEvent* display =
          QueueDisplayEvent(t, TuiDisplayEventKind::ToolUpdate);
      display->ToolCallId = slot->ToolCallId;
      display->ToolUpdateSlot = slotIndex;
      slot->Queued = true;
    }
  }
  Wake(t);
  Mutex_Unlock(&t->Lock);
}

void Tui_ToolEnded(Tui* t, StrView callId, StrView preview, bool isError) {
  Mutex_Lock(&t->Lock);
  TuiDisplayEvent* display = QueueDisplayEvent(t, TuiDisplayEventKind::ToolEnd);
  display->ToolCallId = StrView_Dup(&t->SessionMem, callId);
  display->Payload = StrBuf_Make(&t->SessionMem, preview.Length + 1);
  Sanitize_Display(&display->Payload, preview);
  display->IsError = isError;
  if (TuiToolUpdateSlot* slot = FindToolUpdate(t, callId)) slot->Active = false;
  TuiPerf_ToolEnded(&t->Perf, MonotonicMicros());
  Wake(t);
  Mutex_Unlock(&t->Lock);
}

void Tui_TurnEnded(Tui* t) {
  Mutex_Lock(&t->Lock);
  t->AgentRunning = false;
  t->Sess.RetryStatus = {};
  t->LastEscapeMs = 0;
  t->EscapeArmed = false;
  QueueDisplayEvent(t, TuiDisplayEventKind::AgentEnd);
  Wake(t);
  Mutex_Unlock(&t->Lock);
}

void Tui_RetryStarted(Tui* t, bool providerLevel, u32 delayMs, u32 attempt,
                      u32 maxAttempts) {
  Mutex_Lock(&t->Lock);
  StrBuf status = StrBuf_Make(&t->SessionMem, 96);
  StrBuf_AppendCStr(&status, providerLevel ? "provider request" : "assistant turn");
  StrBuf_AppendCStr(&status, " in ");
  StrBuf_AppendU64(&status, delayMs);
  StrBuf_AppendCStr(&status, " ms (");
  StrBuf_AppendU64(&status, attempt);
  StrBuf_AppendChar(&status, '/');
  StrBuf_AppendU64(&status, maxAttempts);
  StrBuf_AppendChar(&status, ')');
  t->Sess.RetryStatus = StrBuf_View(&status);
  t->State = TuiState::Retry;
  Wake(t);
  Mutex_Unlock(&t->Lock);
}

void Tui_RetryEnded(Tui* t) {
  Mutex_Lock(&t->Lock);
  t->Sess.RetryStatus = {};
  if (t->State == TuiState::Retry) t->State = TuiState::Thinking;
  Wake(t);
  Mutex_Unlock(&t->Lock);
}

void Tui_SetQueuedCount(Tui* t, u32 count) {
  Mutex_Lock(&t->Lock);
  t->QueuedCount = count;
  Wake(t);
  Mutex_Unlock(&t->Lock);
}

void Tui_NoticeDroppedEvents(Tui* t, u32 count) {
  Mutex_Lock(&t->Lock);
  TuiDisplayEvent* display = QueueDisplayEvent(t, TuiDisplayEventKind::Notice);
  StrBuf notice = StrBuf_Make(&t->SessionMem, 64);
  StrBuf_AppendCStr(&notice, "skipped ");
  StrBuf_AppendU64(&notice, count);
  StrBuf_AppendCStr(&notice, " progress updates to keep up");
  display->Payload = notice;
  t->Perf.DroppedEvents = AddSaturated(t->Perf.DroppedEvents, count);
  Wake(t);
  Mutex_Unlock(&t->Lock);
}
