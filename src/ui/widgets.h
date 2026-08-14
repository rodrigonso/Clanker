#pragma once
#include "ui/lineedit.h"
#include "ui/canvas.h"
#include "ui/event.h"

u32 Ui_BeginRow(UiContext* ui, UiKey key, UiLayoutStyle layout = {});
u32 Ui_BeginColumn(UiContext* ui, UiKey key, UiLayoutStyle layout = {});
u32 Ui_BeginStack(UiContext* ui, UiKey key, UiLayoutStyle layout = {});
void Ui_EndContainer(UiContext* ui);

struct UiTextSpan {
  StrView Text;
  Style StyleValue;
};

struct UiTextProps {
  UiLayoutStyle Layout;
  Style StyleValue;
  bool Wrap;
};

u32 Ui_Text(UiContext* ui, UiKey key, StrView text,
            UiTextProps props = {});
u32 Ui_StyledText(UiContext* ui, UiKey key, const UiTextSpan* spans,
                  u32 spanCount, UiTextProps props = {});

struct UiBorderProps {
  UiLayoutStyle Layout;
  Style StyleValue;
  Style FillStyle;
  bool Fill;
};

u32 Ui_BeginBorder(UiContext* ui, UiKey key, UiBorderProps props = {});
u32 Ui_BeginPanel(UiContext* ui, UiKey key, UiBorderProps props = {});

struct UiListState {
  u32 Selected;
  u32 First;
};

using UiListActivateFn = void (*)(void* user, u32 index);

struct UiListProps {
  UiLayoutStyle Layout;
  const StrView* Items;
  u32 Count;
  UiListState* State;
  Style NormalStyle;
  Style SelectedStyle;
  UiListActivateFn OnActivate;
  void* User;
};

u32 Ui_SelectableList(UiContext* ui, UiKey key, UiListProps props);

u32 Ui_BeginScrollView(UiContext* ui, UiKey key, UiScrollState* state,
                       UiLayoutStyle layout = {});
u32 Ui_Scrollbar(UiContext* ui, UiKey key, UiScrollState* state,
                 Style track, Style thumb, UiLayoutStyle layout = {});

using UiTextInputSubmitFn = void (*)(void* user);
using UiTextInputChangeFn = void (*)(void* user);
using UiTextInputInteractFn = void (*)(void* user);
using UiTextInputBoundaryFn = bool (*)(void* user);

struct UiTextInputState {
  LineEdit* Edit;
  u32 FirstRow;
  bool Dragging;
};

struct UiTextInputProps {
  UiLayoutStyle Layout;
  UiTextInputState* State;
  Style TextStyle;
  Style SelectionStyle;
  u16 WrapWidth;
  u16 InsetLeft;
  u16 InsetRight;
  UiTextInputSubmitFn OnSubmit;
  UiTextInputChangeFn OnChange;
  UiTextInputInteractFn OnInteract;
  UiTextInputBoundaryFn OnUpBoundary;
  UiTextInputBoundaryFn OnDownBoundary;
  void* User;
};

u32 Ui_TextInput(UiContext* ui, UiKey key, UiTextInputProps props);
