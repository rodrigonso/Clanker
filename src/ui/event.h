#pragma once
#include "base/str.h"
#include "term/keys.h"
#include "ui/ui.h"

enum class UiEventKind : u8 {
  None,
  Character,
  Key,
  Paste,
  MouseMove,
  MouseDown,
  MouseUp,
  Wheel,
};

struct UiEvent {
  UiEventKind Kind;
  Key KeyData;
  StrView Text;
  i32 X;
  i32 Y;
  i32 Delta;
};

UiEvent Ui_EventFromKey(const Key& key);

void Ui_FinalizeInteractions(UiContext* ui);
bool Ui_Dispatch(UiContext* ui, const UiEvent& event);

void Ui_RequestFocus(UiContext* ui, UiId id);
void Ui_FocusNext(UiContext* ui);
void Ui_FocusPrevious(UiContext* ui);
bool Ui_IsFocused(const UiContext* ui, UiId id);
bool Ui_IsHovered(const UiContext* ui, UiId id);
bool Ui_IsActive(const UiContext* ui, UiId id);

void Ui_CapturePointer(UiContext* ui, UiId id);
void Ui_ReleasePointer(UiContext* ui, UiId id);
void Ui_CancelPointer(UiContext* ui);
