#pragma once
#include "ui/ui.h"

UiRect Ui_Intersect(UiRect a, UiRect b);
bool Ui_RectContains(UiRect rect, i32 x, i32 y);

void Ui_Layout(UiContext* ui, u16 width, u16 height);

