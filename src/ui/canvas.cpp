#include "ui/canvas.h"

#include "ui/event.h"
#include "ui/layout.h"

namespace {

void PaintNode(UiContext* ui, u32 index) {
  UiNode* node = Ui_GetNode(ui, index);
  if (!node || (node->Flags & UI_NODE_HIDDEN) || node->Clip.Width == 0 ||
      node->Clip.Height == 0)
    return;

  if (node->Ops.Paint) {
    UiCanvas canvas{ui, ui->Target, node->Rect, node->Clip};
    node->Ops.Paint(ui, node, &canvas);
  }
  for (u32 child = node->FirstChild; child != UI_NODE_NONE;
       child = ui->Nodes.Data[child].NextSibling)
    PaintNode(ui, child);

}

}  // namespace

void UiCanvas_PutChar(UiCanvas* canvas, i32 x, i32 y, u32 cp, Style style) {
  if (!canvas || !canvas->Target) return;
  i32 screenX = canvas->Bounds.X + x;
  i32 screenY = canvas->Bounds.Y + y;
  u32 width = CharWidth(cp);
  if (width < 1) width = 1;
  if (screenX < canvas->Clip.X || screenY < canvas->Clip.Y ||
      screenY >= canvas->Clip.Y + canvas->Clip.Height ||
      screenX + (i32)width > canvas->Clip.X + canvas->Clip.Width ||
      screenX < 0 || screenY < 0)
    return;
  Screen_PutChar(canvas->Target, (u16)screenX, (u16)screenY, cp, style);
}

u16 UiCanvas_PutText(UiCanvas* canvas, i32 x, i32 y, StrView text,
                     Style style) {
  return UiCanvas_PutTextClipped(
      canvas, x, y, text, style,
      canvas ? (i32)canvas->Bounds.Width : 0);
}

u16 UiCanvas_PutTextClipped(UiCanvas* canvas, i32 x, i32 y, StrView text,
                            Style style, i32 maxXExclusive) {
  if (!canvas || !canvas->Target) return 0;
  i32 localX = x;
  u32 i = 0;
  while (i < text.Length) {
    u32 cp = Utf8Decode(text, &i);
    if (cp == 0) break;
    u32 width = CharWidth(cp);
    if (width < 1) width = 1;
    if (localX + (i32)width > maxXExclusive) break;
    UiCanvas_PutChar(canvas, localX, y, cp, style);
    localX += width;
  }
  return localX > 0 ? (u16)localX : 0;
}

void UiCanvas_Fill(UiCanvas* canvas, UiRect rect, Style style) {
  if (!canvas || !canvas->Target || rect.Width == 0 || rect.Height == 0) return;
  UiRect translated{canvas->Bounds.X + rect.X, canvas->Bounds.Y + rect.Y,
                    rect.Width, rect.Height};
  UiRect clipped = Ui_Intersect(translated, canvas->Clip);
  UiRect target{0, 0, canvas->Target->Cols, canvas->Target->Rows};
  clipped = Ui_Intersect(clipped, target);
  if (clipped.Width == 0 || clipped.Height == 0) return;
  Screen_Fill(canvas->Target, (u16)clipped.X, (u16)clipped.Y,
              (u16)(clipped.X + clipped.Width - 1),
              (u16)(clipped.Y + clipped.Height - 1), style);
}

void UiCanvas_DrawBorder(UiCanvas* canvas, UiRect rect, Style style) {
  if (!canvas || rect.Width < 2 || rect.Height < 2) return;
  i32 lastX = rect.X + rect.Width - 1;
  i32 lastY = rect.Y + rect.Height - 1;
  UiCanvas_PutChar(canvas, rect.X, rect.Y, 0x256d, style);
  UiCanvas_PutChar(canvas, lastX, rect.Y, 0x256e, style);
  UiCanvas_PutChar(canvas, rect.X, lastY, 0x2570, style);
  UiCanvas_PutChar(canvas, lastX, lastY, 0x256f, style);
  for (i32 x = rect.X + 1; x < lastX; x++) {
    UiCanvas_PutChar(canvas, x, rect.Y, 0x2500, style);
    UiCanvas_PutChar(canvas, x, lastY, 0x2500, style);
  }
  for (i32 y = rect.Y + 1; y < lastY; y++) {
    UiCanvas_PutChar(canvas, rect.X, y, 0x2502, style);
    UiCanvas_PutChar(canvas, lastX, y, 0x2502, style);
  }
}

void UiCanvas_TransformStyles(UiCanvas* canvas, UiRect rect,
                              UiStyleTransform transform, void* user) {
  if (!canvas || !canvas->Target || !transform || rect.Width == 0 ||
      rect.Height == 0)
    return;
  UiRect translated{canvas->Bounds.X + rect.X, canvas->Bounds.Y + rect.Y,
                    rect.Width, rect.Height};
  UiRect clipped = Ui_Intersect(translated, canvas->Clip);
  UiRect target{0, 0, canvas->Target->Cols, canvas->Target->Rows};
  clipped = Ui_Intersect(clipped, target);
  for (i32 y = clipped.Y; y < clipped.Y + clipped.Height; y++) {
    for (i32 x = clipped.X; x < clipped.X + clipped.Width; x++) {
      Cell* cell =
          &canvas->Target->Back[(u32)y * canvas->Target->Cols + (u32)x];
      cell->St = transform(cell->St, user);
    }
  }
}

void UiCanvas_SetCursor(UiCanvas* canvas, UiId owner, bool visible, i32 x,
                        i32 y) {
  if (!canvas || !canvas->Ui) return;
  if (!visible) {
    canvas->Ui->CursorRequested = false;
    canvas->Ui->CursorOwner = owner;
    return;
  }
  i32 screenX = canvas->Bounds.X + x;
  i32 screenY = canvas->Bounds.Y + y;
  if (!Ui_RectContains(canvas->Clip, screenX, screenY)) return;
  canvas->Ui->CursorRequested = true;
  canvas->Ui->CursorOwner = owner;
  canvas->Ui->CursorX = screenX;
  canvas->Ui->CursorY = screenY;
}

void Ui_Paint(UiContext* ui) {
  FFI(!ui || ui->Building || !ui->Target);
  ui->CursorRequested = false;
  ui->CursorOwner = 0;
  PaintNode(ui, 0);
  if (ui->CursorRequested)
    Screen_SetCursor(ui->Target, true, (u16)ui->CursorX, (u16)ui->CursorY);
  else
    Screen_SetCursor(ui->Target, false, 0, 0);
}

void Ui_Render(UiContext* ui, u16 width, u16 height) {
  FFI(!ui || !ui->Target);
  Screen_Resize(ui->Target, width, height);
  Screen_BeginFrame(ui->Target);
  Ui_Layout(ui, width, height);
  Ui_FinalizeInteractions(ui);
  Ui_Paint(ui);
}
