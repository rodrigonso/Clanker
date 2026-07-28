#include "ui/event.h"

#include "ui/layout.h"

namespace {

constexpr UiId kRootScope = 0xcbf29ce484222325ull;

bool NodeInScope(const UiContext* ui, u32 index, UiId scope) {
  if (scope == kRootScope) return true;
  while (index != UI_NODE_NONE) {
    const UiNode* node = Ui_GetNode(ui, index);
    if (!node) return false;
    if (node->Id == scope) return true;
    index = node->Parent;
  }
  return false;
}

u32 FindNode(const UiContext* ui, UiId id) {
  if (!id) return UI_NODE_NONE;
  for (u32 i = 0; i < ui->Nodes.Count; i++)
    if (ui->Nodes.Data[i].Id == id) return i;
  return UI_NODE_NONE;
}

bool FocusExists(const UiContext* ui, UiId id, UiId scope) {
  for (u32 i = 0; i < ui->FocusEntries.Count; i++)
    if (ui->FocusEntries.Data[i].Id == id &&
        ui->FocusEntries.Data[i].Scope == scope)
      return true;
  return false;
}

UiId FirstFocus(const UiContext* ui, UiId scope) {
  for (u32 i = 0; i < ui->FocusEntries.Count; i++)
    if (ui->FocusEntries.Data[i].Scope == scope)
      return ui->FocusEntries.Data[i].Id;
  return 0;
}

void CancelCapture(UiContext* ui) {
  if (ui->CaptureCancel)
    ui->CaptureCancel(ui->CaptureCancelUser);
  ui->Captured = 0;
  ui->CaptureCancel = nullptr;
  ui->CaptureCancelUser = nullptr;
}

u32 BuildScopePath(const UiContext* ui, UiId scope, UiId* out) {
  out[0] = kRootScope;
  if (scope == kRootScope) return 1;
  u32 index = FindNode(ui, scope);
  if (index == UI_NODE_NONE) return 1;
  UiId reversed[31];
  u32 count = 0;
  while (index != UI_NODE_NONE && count < 31) {
    const UiNode* node = Ui_GetNode(ui, index);
    if (!node) break;
    if (node->Flags & UI_NODE_FOCUS_SCOPE)
      reversed[count++] = node->Id;
    index = node->Parent;
  }
  for (u32 i = 0; i < count; i++) out[i + 1] = reversed[count - i - 1];
  return count + 1;
}

void Collect(UiContext* ui, u32 index, UiId scope) {
  UiNode* node = Ui_GetNode(ui, index);
  if (!node || (node->Flags & (UI_NODE_HIDDEN | UI_NODE_DISABLED))) return;
  if (node->Flags & UI_NODE_FOCUS_SCOPE) scope = node->Id;
  if (node->Ops.Event && node->Clip.Width && node->Clip.Height)
    Array_Push(&ui->HitRegions,
               UiHitRegion{node->Id, scope, index, node->Clip});
  if ((node->Flags & UI_NODE_FOCUSABLE) && node->Ops.Event)
    Array_Push(&ui->FocusEntries, UiFocusEntry{node->Id, scope, index});
  for (u32 child = node->FirstChild; child != UI_NODE_NONE;
       child = ui->Nodes.Data[child].NextSibling)
    Collect(ui, child, scope);
}

u32 HitTest(const UiContext* ui, i32 x, i32 y) {
  for (u32 i = ui->HitRegions.Count; i > 0; i--) {
    const UiHitRegion& hit = ui->HitRegions.Data[i - 1];
    if (hit.Scope == ui->ActiveScope && Ui_RectContains(hit.Rect, x, y))
      return hit.Node;
  }
  return UI_NODE_NONE;
}

bool DispatchTo(UiContext* ui, u32 index, const UiEvent& event) {
  while (index != UI_NODE_NONE) {
    UiNode* node = Ui_GetNode(ui, index);
    if (!node || !NodeInScope(ui, index, ui->ActiveScope)) return false;
    if (!(node->Flags & (UI_NODE_HIDDEN | UI_NODE_DISABLED)) &&
        node->Ops.Event && node->Ops.Event(ui, node, event))
      return true;
    index = node->Parent;
  }
  return false;
}

void MoveFocus(UiContext* ui, i32 direction) {
  u32 count = 0;
  u32 current = UI_NODE_NONE;
  for (u32 i = 0; i < ui->FocusEntries.Count; i++) {
    const UiFocusEntry& entry = ui->FocusEntries.Data[i];
    if (entry.Scope != ui->ActiveScope) continue;
    if (entry.Id == ui->Focused) current = count;
    count++;
  }
  if (count == 0) {
    ui->Focused = 0;
    return;
  }

  u32 target;
  if (current == UI_NODE_NONE)
    target = direction > 0 ? 0 : count - 1;
  else if (direction > 0)
    target = (current + 1) % count;
  else
    target = current == 0 ? count - 1 : current - 1;

  u32 at = 0;
  for (u32 i = 0; i < ui->FocusEntries.Count; i++) {
    const UiFocusEntry& entry = ui->FocusEntries.Data[i];
    if (entry.Scope != ui->ActiveScope) continue;
    if (at++ == target) {
      ui->Focused = entry.Id;
      if (ui->ScopeDepth)
        ui->ScopeStack[ui->ScopeDepth - 1].Focus = entry.Id;
      return;
    }
  }
}

}  // namespace

UiEvent Ui_EventFromKey(const Key& key) {
  UiEvent event = {};
  event.KeyData = key;
  event.X = key.MouseX;
  event.Y = key.MouseY;
  switch (key.Kind) {
    case KeyKind::Char:
      event.Kind = UiEventKind::Character;
      break;
    case KeyKind::MouseMove:
      event.Kind = UiEventKind::MouseMove;
      break;
    case KeyKind::MouseLeftDown:
      event.Kind = UiEventKind::MouseDown;
      break;
    case KeyKind::MouseLeftUp:
      event.Kind = UiEventKind::MouseUp;
      break;
    case KeyKind::ScrollUp:
      event.Kind = UiEventKind::Wheel;
      event.Delta = -1;
      break;
    case KeyKind::ScrollDown:
      event.Kind = UiEventKind::Wheel;
      event.Delta = 1;
      break;
    case KeyKind::None:
    case KeyKind::PasteStart:
    case KeyKind::PasteEnd:
      event.Kind = UiEventKind::None;
      break;
    default:
      event.Kind = UiEventKind::Key;
      break;
  }
  return event;
}

void Ui_FinalizeInteractions(UiContext* ui) {
  FFI(!ui || ui->Building || ui->Nodes.Count == 0);
  Array_Clear(&ui->HitRegions);
  Array_Clear(&ui->FocusEntries);
  Collect(ui, 0, kRootScope);

  UiId newScope = ui->RequestedScope ? ui->RequestedScope : kRootScope;
  if (newScope != kRootScope && FindNode(ui, newScope) == UI_NODE_NONE)
    newScope = kRootScope;

  if (ui->ScopeDepth == 0) {
    ui->ScopeStack[0] = UiScopeState{kRootScope, ui->Focused};
    ui->ScopeDepth = 1;
  } else {
    ui->ScopeStack[ui->ScopeDepth - 1].Focus = ui->Focused;
  }

  UiId path[32];
  u32 pathDepth = BuildScopePath(ui, newScope, path);
  u32 common = 0;
  while (common < ui->ScopeDepth && common < pathDepth &&
         ui->ScopeStack[common].Scope == path[common])
    common++;
  bool scopeChanged =
      ui->ScopeDepth != pathDepth || common != pathDepth;
  if (scopeChanged) {
    if (ui->Captured) CancelCapture(ui);
    ui->ScopeDepth = common;
    while (ui->ScopeDepth < pathDepth) {
      ui->ScopeStack[ui->ScopeDepth] =
          UiScopeState{path[ui->ScopeDepth], 0};
      ui->ScopeDepth++;
    }
    ui->ActiveScope = path[pathDepth - 1];
    ui->Focused = ui->ScopeStack[pathDepth - 1].Focus;
    ui->Hovered = 0;
    ui->Active = 0;
  }
  if (ui->ActiveScope == 0)
    ui->ActiveScope = ui->ScopeStack[ui->ScopeDepth - 1].Scope;
  if (!FocusExists(ui, ui->Focused, ui->ActiveScope))
    ui->Focused = FirstFocus(ui, ui->ActiveScope);
  ui->ScopeStack[ui->ScopeDepth - 1].Focus = ui->Focused;
  if (FindNode(ui, ui->Hovered) == UI_NODE_NONE) ui->Hovered = 0;
  if (FindNode(ui, ui->Active) == UI_NODE_NONE) ui->Active = 0;
  if (ui->Captured) {
    u32 captured = FindNode(ui, ui->Captured);
    if (captured == UI_NODE_NONE ||
        (ui->Nodes.Data[captured].Flags &
         (UI_NODE_HIDDEN | UI_NODE_DISABLED)) ||
        !NodeInScope(ui, captured, ui->ActiveScope))
      CancelCapture(ui);
  }
}

bool Ui_Dispatch(UiContext* ui, const UiEvent& event) {
  if (!ui || ui->Building || event.Kind == UiEventKind::None) return false;
  if (event.Kind == UiEventKind::Key &&
      event.KeyData.Kind == KeyKind::Tab) {
    if (event.KeyData.Shift)
      Ui_FocusPrevious(ui);
    else
      Ui_FocusNext(ui);
    return true;
  }

  if (event.Kind == UiEventKind::MouseMove) {
    u32 hit = HitTest(ui, event.X, event.Y);
    ui->Hovered = hit == UI_NODE_NONE ? 0 : ui->Nodes.Data[hit].Id;
    u32 target = FindNode(ui, ui->Captured);
    if (target == UI_NODE_NONE) target = hit;
    return target != UI_NODE_NONE && DispatchTo(ui, target, event);
  }

  if (event.Kind == UiEventKind::MouseDown) {
    u32 hit = HitTest(ui, event.X, event.Y);
    ui->Hovered = hit == UI_NODE_NONE ? 0 : ui->Nodes.Data[hit].Id;
    ui->Active = ui->Hovered;
    if (hit != UI_NODE_NONE &&
        (ui->Nodes.Data[hit].Flags & UI_NODE_FOCUSABLE))
      Ui_RequestFocus(ui, ui->Nodes.Data[hit].Id);
    return hit != UI_NODE_NONE && DispatchTo(ui, hit, event);
  }

  if (event.Kind == UiEventKind::MouseUp) {
    u32 target = FindNode(ui, ui->Captured);
    if (target == UI_NODE_NONE) target = HitTest(ui, event.X, event.Y);
    bool handled =
        target != UI_NODE_NONE && DispatchTo(ui, target, event);
    ui->Active = 0;
    CancelCapture(ui);
    return handled;
  }

  if (event.Kind == UiEventKind::Wheel) {
    u32 target = HitTest(ui, event.X, event.Y);
    return target != UI_NODE_NONE && DispatchTo(ui, target, event);
  }

  u32 focused = FindNode(ui, ui->Focused);
  if (focused != UI_NODE_NONE) return DispatchTo(ui, focused, event);
  u32 scope = FindNode(ui, ui->ActiveScope);
  return scope != UI_NODE_NONE && DispatchTo(ui, scope, event);
}

void Ui_RequestFocus(UiContext* ui, UiId id) {
  if (!ui || !FocusExists(ui, id, ui->ActiveScope)) return;
  ui->Focused = id;
  if (ui->ScopeDepth) ui->ScopeStack[ui->ScopeDepth - 1].Focus = id;
}

void Ui_FocusNext(UiContext* ui) {
  if (ui) MoveFocus(ui, 1);
}

void Ui_FocusPrevious(UiContext* ui) {
  if (ui) MoveFocus(ui, -1);
}

bool Ui_IsFocused(const UiContext* ui, UiId id) {
  return ui && ui->Focused == id;
}

bool Ui_IsHovered(const UiContext* ui, UiId id) {
  return ui && ui->Hovered == id;
}

bool Ui_IsActive(const UiContext* ui, UiId id) {
  return ui && ui->Active == id;
}

void Ui_CapturePointer(UiContext* ui, UiId id) {
  if (!ui) return;
  u32 index = FindNode(ui, id);
  if (index == UI_NODE_NONE ||
      (ui->Nodes.Data[index].Flags &
       (UI_NODE_HIDDEN | UI_NODE_DISABLED)) ||
      !NodeInScope(ui, index, ui->ActiveScope))
    return;
  if (ui->Captured && ui->Captured != id) CancelCapture(ui);
  ui->Captured = id;
  ui->CaptureCancel = ui->Nodes.Data[index].Ops.Cancel;
  ui->CaptureCancelUser = ui->Nodes.Data[index].Persistent;
}

void Ui_ReleasePointer(UiContext* ui, UiId id) {
  if (!ui || ui->Captured != id) return;
  ui->Captured = 0;
  ui->CaptureCancel = nullptr;
  ui->CaptureCancelUser = nullptr;
}

void Ui_CancelPointer(UiContext* ui) {
  if (ui && ui->Captured) CancelCapture(ui);
}
