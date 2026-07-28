#pragma once
#include "term/screen.h"
#include "ui/ui.h"

struct UiCanvas {
  UiContext* Ui;
  Screen* Target;
  UiRect Bounds;
  UiRect Clip;
};

using UiStyleTransform = Style (*)(Style current, void* user);

void UiCanvas_PutChar(UiCanvas* canvas, i32 x, i32 y, u32 cp, Style style);
u16 UiCanvas_PutText(UiCanvas* canvas, i32 x, i32 y, StrView text,
                     Style style);
u16 UiCanvas_PutTextClipped(UiCanvas* canvas, i32 x, i32 y, StrView text,
                            Style style, i32 maxXExclusive);
void UiCanvas_Fill(UiCanvas* canvas, UiRect rect, Style style);
void UiCanvas_DrawBorder(UiCanvas* canvas, UiRect rect, Style style);
void UiCanvas_TransformStyles(UiCanvas* canvas, UiRect rect,
                              UiStyleTransform transform, void* user);
void UiCanvas_SetCursor(UiCanvas* canvas, UiId owner, bool visible, i32 x,
                        i32 y);

void Ui_Paint(UiContext* ui);
void Ui_Render(UiContext* ui, u16 width, u16 height);
