#include "tui/internal.h"

// Transcript text selection and the clipboard path it feeds. Selection is
// keyed to a LineModel revision, so everything that reads it has to cope
// with the model having been rebuilt underneath.

namespace TuiInternal {

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

}  // namespace TuiInternal
