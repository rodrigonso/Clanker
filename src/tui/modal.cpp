#include "tui/internal.h"

// Modal selection and text entry: the overlay itself, the keys it consumes
// while it owns input, and the blocking helpers callers use to drive it.

namespace TuiInternal {

bool ModalOptionMatches(const Tui* t, u32 optionIndex) {
  if (!t || optionIndex >= t->Sess.ModalOptionCount) return false;
  return StrView_ContainsInsensitiveAscii(
      t->Sess.ModalOptions[optionIndex], LineEdit_Text(&t->ModalEdit));
}

u32 ModalMatchCount(const Tui* t) {
  u32 count = 0;
  for (u32 i = 0; i < t->Sess.ModalOptionCount; i++)
    if (ModalOptionMatches(t, i)) count++;
  return count;
}

bool ModalMatchAt(const Tui* t, u32 matchIndex, u32* optionIndex) {
  for (u32 i = 0; i < t->Sess.ModalOptionCount; i++) {
    if (!ModalOptionMatches(t, i)) continue;
    if (matchIndex == 0) {
      if (optionIndex) *optionIndex = i;
      return true;
    }
    matchIndex--;
  }
  return false;
}

void ResetModalMatches(Tui* t) {
  t->ModalSelected = 0;
  t->ModalFirst = 0;
}

void PaintModalEdit(const UiNode* node, UiCanvas* canvas, Tui* t, u16 textX,
                    u16 textEnd, u16 editY, Style edit) {
  StrView text = LineEdit_Text(&t->ModalEdit);
  u32 avail = textEnd > textX ? (u32)(textEnd - textX) : 0;
  u32 cursorW = TextWidth(StrView{text.Data, LineEdit_Cursor(&t->ModalEdit)});
  u32 skip = 0;
  u32 offset = 0;
  if (avail && cursorW >= avail) {
    u32 want = cursorW - avail + 1;
    while (offset < text.Length && skip < want)
      skip += CharCellWidth(Utf8Decode(text, &offset));
  }
  UiCanvas_PutTextClipped(canvas, textX, editY,
                          StrView{text.Data + offset, text.Length - offset},
                          edit, textEnd);
  u16 cursorX = (u16)(textX + (cursorW - skip));
  if (avail == 0)
    cursorX = textX;
  else if (cursorX >= textEnd)
    cursorX = (u16)(textEnd - 1);
  UiCanvas_SetCursor(canvas, node->Id, true, cursorX, editY);
}

void PaintModalNode(UiContext*, const UiNode* node, UiCanvas* canvas) {
  ModalComponentData* data = (ModalComponentData*)node->User;
  Tui* t = data->Controller;
  if (t->ModalKind == TuiModalKind::None ||
      t->ModalResult != TuiModalResult::Pending || data->TopBorderY < 4)
    return;

  u16 panelW = data->Cols > 76 ? 72 : (u16)(data->Cols - 2);
  u32 matchCount =
      t->ModalKind == TuiModalKind::Select ? ModalMatchCount(t) : 0;
  u32 selectRows = matchCount ? matchCount : 1;
  u16 desiredH = t->ModalKind == TuiModalKind::Select
                     ? (u16)((selectRows < 12 ? selectRows : 12) + 3)
                     : 7;
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
  u16 titleX = (u16)(x0 + 2);
  if (t->Sess.ModalCategory.Length) {
    titleX = UiCanvas_PutTextClipped(canvas, titleX, (u16)(y0 + 1),
                                     t->Sess.ModalCategory, path, (u16)(x1 - 1));
    titleX = UiCanvas_PutTextClipped(canvas, titleX, (u16)(y0 + 1), SVL("  /  "),
                                     path, (u16)(x1 - 1));
  }
  Style title = panel;
  title.Attr |= ATTR_BOLD;
  UiCanvas_PutTextClipped(
      canvas, titleX, (u16)(y0 + 1),
      compactText ? t->Sess.ModalPrompt : t->Sess.ModalTitle, title,
      (u16)(x1 - 1));

  if (t->ModalKind == TuiModalKind::Select) {
    u32 visible = panelH > 3 ? panelH - 3 : 1;
    if (matchCount == 0) {
      ResetModalMatches(t);
      Style empty = panel;
      empty.Attr |= ATTR_DIM;
      UiCanvas_PutTextClipped(canvas, (u16)(x0 + 2), (u16)(y0 + 2),
                              SVL("No matching options"), empty,
                              (u16)(x1 - 1));
      UiCanvas_SetCursor(canvas, node->Id, false, 0, 0);
      return;
    }
    if (t->ModalSelected >= matchCount) t->ModalSelected = 0;
    if (t->ModalSelected < t->ModalFirst) t->ModalFirst = t->ModalSelected;
    if (visible && t->ModalSelected >= t->ModalFirst + visible)
      t->ModalFirst = t->ModalSelected - visible + 1;
    u32 maxFirst = matchCount > visible ? matchCount - visible : 0;
    if (t->ModalFirst > maxFirst) t->ModalFirst = maxFirst;
    for (u32 row = 0; row < visible; row++) {
      u32 matchIndex = t->ModalFirst + row;
      u32 optionIndex = 0;
      if (!ModalMatchAt(t, matchIndex, &optionIndex)) break;
      u16 y = (u16)(y0 + 2 + row);
      bool selected = matchIndex == t->ModalSelected;
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
                              t->Sess.ModalOptions[optionIndex], option,
                              (u16)(x1 - 1));
    }
    UiCanvas_SetCursor(canvas, node->Id, false, 0, 0);
    return;
  }

  Style prompt = panel;
  prompt.Attr |= ATTR_DIM;
  // The composer is a bordered box with plain text inside, so the answer field
  // matches it. That needs three rows; below that fall back to the single
  // reversed row, which still gives a short terminal a usable field.
  bool boxed = panelH >= 6;
  u16 boxTop = (u16)(y1 - 3);
  if (!compactText && (!boxed || boxTop > (u16)(y0 + 2)))
    UiCanvas_PutTextClipped(canvas, (u16)(x0 + 2), (u16)(y0 + 2),
                            t->Sess.ModalPrompt, prompt, (u16)(x1 - 1));

  Style edit = panel;
  u16 editY;
  u16 textX;
  u16 textEnd;  // exclusive
  if (boxed) {
    UiRect box{(u16)(x0 + 2), boxTop, (u16)(x1 - x0 - 3), 3};
    UiCanvas_Fill(canvas, box, panel);
    UiCanvas_DrawBorder(canvas, box, TuiSt(t, 250, ATTR_NONE));
    editY = (u16)(boxTop + 1);
    textX = (u16)(x0 + 4);
    textEnd = (u16)(x1 - 3);
  } else {
    edit.Attr |= ATTR_REVERSE;
    editY = (u16)(y1 - 1);
    UiCanvas_Fill(canvas, UiRect{(u16)(x0 + 2), editY, (u16)(x1 - x0 - 3), 1},
                  edit);
    textX = (u16)(x0 + 3);
    textEnd = (u16)(x1 - 2);
  }

  // One row, so the field pans horizontally to keep the caret visible rather
  // than wrapping the way the composer does.
  PaintModalEdit(node, canvas, t, textX, textEnd, editY, edit);
}

void FinishModal(Tui* t, TuiModalResult result) {
  t->ModalResult = result;
  // The modal is no longer on screen. Leaving Kind set made it look permanently
  // open, which silently disabled session-memory compaction for the rest of the
  // process after the first picker the user confirmed.
  t->ModalKind = TuiModalKind::None;
  Cond_Broadcast(&t->ModalCv);
}

void RequestStopLocked(Tui* t) {
  Atomic_StoreI32(&t->AbortFlag, 1);
  Atomic_StoreI32(&t->OperationAbortFlag, 1);
  t->Quit = true;
  t->Stop = true;
  if (t->ModalResult == TuiModalResult::Pending)
    t->ModalResult = TuiModalResult::Cancelled;
  Cond_Signal(&t->SubmitCv);
  Cond_Broadcast(&t->ModalCv);
  Wake(t);
}

void HandleSelectKey(Tui* t, const Key& k) {
  u32 matchCount = ModalMatchCount(t);
  switch (k.Kind) {
    case KeyKind::Char:
      if (!k.Alt && k.Codepoint >= 0x20) {
        LineEdit_InsertChar(&t->ModalEdit, k.Codepoint);
        ResetModalMatches(t);
      }
      break;
    case KeyKind::Up:
      if (t->ModalSelected > 0) t->ModalSelected--;
      break;
    case KeyKind::Down:
      if (t->ModalSelected + 1 < matchCount) t->ModalSelected++;
      break;
    case KeyKind::Home: t->ModalSelected = 0; break;
    case KeyKind::End:
      if (matchCount) t->ModalSelected = matchCount - 1;
      break;
    case KeyKind::Left: LineEdit_Left(&t->ModalEdit); break;
    case KeyKind::Right: LineEdit_Right(&t->ModalEdit); break;
    case KeyKind::WordLeft: LineEdit_WordLeft(&t->ModalEdit); break;
    case KeyKind::WordRight: LineEdit_WordRight(&t->ModalEdit); break;
    case KeyKind::Delete:
      LineEdit_Delete(&t->ModalEdit);
      ResetModalMatches(t);
      break;
    case KeyKind::CtrlW:
      LineEdit_DeleteWordBack(&t->ModalEdit);
      ResetModalMatches(t);
      break;
    case KeyKind::CtrlU:
      LineEdit_KillToStart(&t->ModalEdit);
      ResetModalMatches(t);
      break;
    case KeyKind::CtrlK:
      LineEdit_KillToEnd(&t->ModalEdit);
      ResetModalMatches(t);
      break;
    case KeyKind::Enter: {
      u32 optionIndex = 0;
      if (!ModalMatchAt(t, t->ModalSelected, &optionIndex)) break;
      t->ModalSelected = optionIndex;
      FinishModal(t, TuiModalResult::Confirmed);
      break;
    }
    case KeyKind::Backspace:
      if (LineEdit_Empty(&t->ModalEdit))
        FinishModal(t, TuiModalResult::Back);
      else {
        LineEdit_Backspace(&t->ModalEdit);
        ResetModalMatches(t);
      }
      break;
    case KeyKind::Escape: FinishModal(t, TuiModalResult::Cancelled); break;
    case KeyKind::CtrlC:
      if (t->OperationActive) Atomic_StoreI32(&t->OperationAbortFlag, 1);
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
      t->Sess.ModalTextResult =
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
      if (t->OperationActive) Atomic_StoreI32(&t->OperationAbortFlag, 1);
      FinishModal(t, TuiModalResult::Cancelled);
      break;
    default: break;
  }
}

}  // namespace TuiInternal

using namespace TuiInternal;

bool Tui_BeginSelection(Tui* t, StrView title, const StrView* options, u32 optionCount,
                        u32 initialSelection, StrView category) {
  if (!options || optionCount == 0) return false;
  Mutex_Lock(&t->Lock);
  if (t->Stop || t->Quit || t->ModalResult == TuiModalResult::Pending) {
    Mutex_Unlock(&t->Lock);
    return false;
  }
  Ui_CancelPointer(&t->Ui);
  LineEdit_Clear(&t->ModalEdit);
  t->Sess.ModalOptions = ArenaPushArray(&t->SessionMem, StrView, optionCount);
  for (u32 i = 0; i < optionCount; i++)
    t->Sess.ModalOptions[i] = StrView_Dup(&t->SessionMem, options[i]);
  t->Sess.ModalOptionCount = optionCount;
  t->ModalSelected = initialSelection < optionCount ? initialSelection : optionCount - 1;
  t->ModalFirst = 0;
  t->Sess.ModalTitle = StrView_Dup(&t->SessionMem, title);
  t->Sess.ModalCategory = StrView_Dup(&t->SessionMem, category);
  t->Sess.ModalPrompt = {};
  t->Sess.ModalTextResult = {};
  t->ModalKind = TuiModalKind::Select;
  t->ModalResult = TuiModalResult::Pending;
  Wake(t);
  Mutex_Unlock(&t->Lock);
  return true;
}

bool Tui_BeginTextEntry(Tui* t, StrView title, StrView prompt, StrView initial,
                        StrView category) {
  Mutex_Lock(&t->Lock);
  if (t->Stop || t->Quit || t->ModalResult == TuiModalResult::Pending) {
    Mutex_Unlock(&t->Lock);
    return false;
  }
  Ui_CancelPointer(&t->Ui);
  LineEdit_Clear(&t->ModalEdit);
  LineEdit_InsertText(&t->ModalEdit, initial);
  t->Sess.ModalTitle = StrView_Dup(&t->SessionMem, title);
  t->Sess.ModalCategory = StrView_Dup(&t->SessionMem, category);
  t->Sess.ModalPrompt = StrView_Dup(&t->SessionMem, prompt);
  t->Sess.ModalOptions = nullptr;
  t->Sess.ModalOptionCount = 0;
  t->ModalSelected = 0;
  t->ModalFirst = 0;
  t->Sess.ModalTextResult = {};
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
  if (text) *text = t->Sess.ModalTextResult;
  Mutex_Unlock(&t->Lock);
  return result;
}

TuiModalResult Tui_Select(Tui* t, StrView title, const StrView* options,
                         u32 optionCount, u32* selected,
                         u32 initialSelection, StrView category) {
  if (!Tui_BeginSelection(t, title, options, optionCount, initialSelection, category))
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
                           StrView initial, StrView* out, StrView category) {
  if (!Tui_BeginTextEntry(t, title, prompt, initial, category))
    return TuiModalResult::Cancelled;
  Mutex_Lock(&t->Lock);
  while (t->ModalResult == TuiModalResult::Pending && !t->Stop)
    Cond_Wait(&t->ModalCv, &t->Lock);
  TuiModalResult result = t->ModalResult;
  if (out)
    *out = result == TuiModalResult::Confirmed ? t->Sess.ModalTextResult : StrView{};
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
