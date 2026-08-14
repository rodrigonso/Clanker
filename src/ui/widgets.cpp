#include "ui/widgets.h"

#include <string.h>

namespace {

struct TextData {
  UiTextSpan* Spans;
  u32 Count;
  bool Wrap;
};

struct TextMetrics {
  u16 Width;
  u16 Height;
};

TextMetrics MeasureSpans(const UiTextSpan* spans, u32 count, u16 maxWidth,
                         bool wrap) {
  if (maxWidth == 0) maxWidth = 1;
  u32 rowWidth = 0;
  u32 widest = 0;
  u32 rows = 1;
  for (u32 span = 0; span < count; span++) {
    u32 offset = 0;
    while (offset < spans[span].Text.Length) {
      u32 cp = Utf8Decode(spans[span].Text, &offset);
      if (cp == '\n') {
        if (rowWidth > widest) widest = rowWidth;
        rowWidth = 0;
        rows++;
        continue;
      }
      u32 width = CharCellWidth(cp);
      if (wrap && rowWidth && rowWidth + width > maxWidth) {
        if (rowWidth > widest) widest = rowWidth;
        rowWidth = 0;
        rows++;
      }
      rowWidth += width;
    }
  }
  if (rowWidth > widest) widest = rowWidth;
  if (wrap && widest > maxWidth) widest = maxWidth;
  if (widest > 0xffffu) widest = 0xffffu;
  if (rows > 0xffffu) rows = 0xffffu;
  return TextMetrics{(u16)widest, (u16)rows};
}

UiSize MeasureText(UiContext*, UiNode* node, UiConstraints constraints) {
  TextData* data = (TextData*)node->User;
  TextMetrics metrics =
      MeasureSpans(data->Spans, data->Count, constraints.MaxWidth, data->Wrap);
  return UiSize{metrics.Width, metrics.Height};
}

void PaintText(UiContext*, const UiNode* node, UiCanvas* canvas) {
  TextData* data = (TextData*)node->User;
  i32 x = 0;
  i32 y = 0;
  u16 width = node->Rect.Width ? node->Rect.Width : 1;
  for (u32 span = 0; span < data->Count; span++) {
    u32 offset = 0;
    while (offset < data->Spans[span].Text.Length) {
      u32 cp = Utf8Decode(data->Spans[span].Text, &offset);
      if (cp == '\n') {
        x = 0;
        y++;
        continue;
      }
      u32 cellWidth = CharCellWidth(cp);
      if (data->Wrap && x && x + (i32)cellWidth > width) {
        x = 0;
        y++;
      }
      if (y >= node->Rect.Height) return;
      UiCanvas_PutChar(canvas, x, y, cp, data->Spans[span].StyleValue);
      x += cellWidth;
    }
  }
}

struct BorderData {
  Style Border;
  Style Fill;
  bool FillEnabled;
};

void PaintBorder(UiContext*, const UiNode* node, UiCanvas* canvas) {
  BorderData* data = (BorderData*)node->User;
  if (node->Rect.Width == 0 || node->Rect.Height == 0) return;
  if (data->FillEnabled)
    UiCanvas_Fill(canvas, UiRect{0, 0, node->Rect.Width, node->Rect.Height},
                  data->Fill);
  UiCanvas_DrawBorder(
      canvas, UiRect{0, 0, node->Rect.Width, node->Rect.Height},
      data->Border);
}

struct ListData {
  UiListProps Props;
};

UiSize MeasureList(UiContext*, UiNode* node, UiConstraints constraints) {
  ListData* data = (ListData*)node->User;
  u32 widest = 0;
  for (u32 i = 0; i < data->Props.Count; i++) {
    u32 width = TextWidth(data->Props.Items[i]) + 2;
    if (width > widest) widest = width;
  }
  if (widest > constraints.MaxWidth) widest = constraints.MaxWidth;
  u32 height = data->Props.Count;
  if (height > constraints.MaxHeight) height = constraints.MaxHeight;
  return UiSize{(u16)widest, (u16)height};
}

void NormalizeList(ListData* data, u16 height) {
  UiListState* state = data->Props.State;
  if (!state || data->Props.Count == 0) return;
  if (state->Selected >= data->Props.Count)
    state->Selected = data->Props.Count - 1;
  u32 visible = height ? height : 1;
  if (state->Selected < state->First) state->First = state->Selected;
  if (state->Selected >= state->First + visible)
    state->First = state->Selected - visible + 1;
  u32 maxFirst =
      data->Props.Count > visible ? data->Props.Count - visible : 0;
  if (state->First > maxFirst) state->First = maxFirst;
}

void PaintList(UiContext*, const UiNode* node, UiCanvas* canvas) {
  ListData* data = (ListData*)node->User;
  NormalizeList(data, node->Rect.Height);
  UiListState* state = data->Props.State;
  if (!state) return;
  for (u16 row = 0; row < node->Rect.Height; row++) {
    u32 index = state->First + row;
    if (index >= data->Props.Count) break;
    bool selected = index == state->Selected;
    Style style =
        selected ? data->Props.SelectedStyle : data->Props.NormalStyle;
    if (selected)
      UiCanvas_Fill(canvas, UiRect{0, row, node->Rect.Width, 1}, style);
    UiCanvas_PutText(canvas, 0, row, selected ? SVL("\xe2\x80\xba ") : SVL("  "),
                     style);
    UiCanvas_PutText(canvas, 2, row, data->Props.Items[index], style);
  }
}

bool EventList(UiContext* ui, UiNode* node, const UiEvent& event) {
  ListData* data = (ListData*)node->User;
  UiListState* state = data->Props.State;
  if (!state || data->Props.Count == 0) return false;
  bool activate = false;
  if (event.Kind == UiEventKind::Key) {
    switch (event.KeyData.Kind) {
      case KeyKind::Up:
        if (state->Selected > 0) state->Selected--;
        break;
      case KeyKind::Down:
        if (state->Selected + 1 < data->Props.Count) state->Selected++;
        break;
      case KeyKind::Home:
        state->Selected = 0;
        break;
      case KeyKind::End:
        state->Selected = data->Props.Count - 1;
        break;
      case KeyKind::Enter:
        activate = true;
        break;
      default:
        return false;
    }
  } else if (event.Kind == UiEventKind::MouseDown) {
    i32 row = event.Y - node->Rect.Y;
    if (row < 0 || row >= node->Rect.Height) return false;
    u32 index = state->First + (u32)row;
    if (index >= data->Props.Count) return false;
    state->Selected = index;
  } else if (event.Kind == UiEventKind::MouseUp &&
             Ui_IsActive(ui, node->Id)) {
    activate = true;
  } else {
    return false;
  }
  NormalizeList(data, node->Rect.Height);
  if (activate && data->Props.OnActivate)
    data->Props.OnActivate(data->Props.User, state->Selected);
  return true;
}

bool EventScrollView(UiContext*, UiNode* node, const UiEvent& event) {
  UiScrollState* state = (UiScrollState*)node->User;
  if (!state || event.Kind != UiEventKind::Wheel) return false;
  state->Offset += event.Delta * 3;
  if (state->Offset < 0) state->Offset = 0;
  if (state->Offset > state->MaxOffset) state->Offset = state->MaxOffset;
  state->Follow = state->Offset >= state->MaxOffset;
  return true;
}

struct ScrollbarData {
  UiScrollState* State;
  Style Track;
  Style Thumb;
};

struct ThumbGeometry {
  u16 Top;
  u16 Height;
};

ThumbGeometry ScrollThumb(const UiNode* node, const UiScrollState* state) {
  if (!state || state->MaxOffset <= 0 || node->Rect.Height == 0) return {};
  u32 viewport = node->Rect.Height;
  u32 total = viewport + state->MaxOffset;
  u32 height = (u32)(((u64)viewport * viewport) / total);
  if (height < 1) height = 1;
  if (height > viewport) height = viewport;
  u32 travel = viewport - height;
  u32 top = state->MaxOffset
                ? (u32)(((u64)state->Offset * travel) / state->MaxOffset)
                : 0;
  return ThumbGeometry{(u16)top, (u16)height};
}

void PaintScrollbar(UiContext*, const UiNode* node, UiCanvas* canvas) {
  ScrollbarData* data = (ScrollbarData*)node->User;
  if (!data->State || data->State->MaxOffset <= 0) return;
  ThumbGeometry thumb = ScrollThumb(node, data->State);
  for (u16 y = 0; y < node->Rect.Height; y++)
    UiCanvas_PutChar(canvas, 0, y, 0x2595, data->Track);
  for (u16 y = 0; y < thumb.Height; y++)
    UiCanvas_PutChar(canvas, 0, thumb.Top + y, 0x2595, data->Thumb);
}

void ScrollbarTo(UiNode* node, ScrollbarData* data, i32 thumbTop) {
  ThumbGeometry thumb = ScrollThumb(node, data->State);
  i32 travel = node->Rect.Height - thumb.Height;
  if (travel <= 0) return;
  if (thumbTop < 0) thumbTop = 0;
  if (thumbTop > travel) thumbTop = travel;
  data->State->Offset =
      (i32)(((i64)thumbTop * data->State->MaxOffset + travel / 2) / travel);
  data->State->Follow = data->State->Offset >= data->State->MaxOffset;
}

bool EventScrollbar(UiContext* ui, UiNode* node, const UiEvent& event) {
  ScrollbarData* data = (ScrollbarData*)node->User;
  if (!data->State || data->State->MaxOffset <= 0) return false;
  i32 y = event.Y - node->Rect.Y;
  ThumbGeometry thumb = ScrollThumb(node, data->State);
  if (event.Kind == UiEventKind::MouseDown) {
    if (y >= thumb.Top && y < thumb.Top + thumb.Height)
      data->State->DragOffset = y - thumb.Top;
    else {
      data->State->DragOffset = thumb.Height / 2;
      ScrollbarTo(node, data, y - data->State->DragOffset);
    }
    data->State->Dragging = true;
    Ui_CapturePointer(ui, node->Id);
    return true;
  }
  if (event.Kind == UiEventKind::MouseMove && data->State->Dragging) {
    ScrollbarTo(node, data, y - data->State->DragOffset);
    return true;
  }
  if (event.Kind == UiEventKind::MouseUp && data->State->Dragging) {
    ScrollbarTo(node, data, y - data->State->DragOffset);
    data->State->Dragging = false;
    Ui_ReleasePointer(ui, node->Id);
    return true;
  }
  return false;
}

void CancelScrollbar(void* persistent) {
  UiScrollState* state = (UiScrollState*)persistent;
  if (state) state->Dragging = false;
}

u32 InputTextWidth(StrView text) {
  u32 width = 0;
  u32 offset = 0;
  while (offset < text.Length) width += CharCellWidth(Utf8Decode(text, &offset));
  return width;
}

struct InputData {
  UiTextInputProps Props;
};

UiSize MeasureInput(UiContext*, UiNode* node, UiConstraints constraints) {
  InputData* data = (InputData*)node->User;
  if (!data->Props.State || !data->Props.State->Edit) return UiSize{1, 1};
  u16 contentMax =
      constraints.MaxWidth >
              (u32)data->Props.InsetLeft + data->Props.InsetRight
          ? (u16)(constraints.MaxWidth - data->Props.InsetLeft -
                  data->Props.InsetRight)
          : 1;
  LineEditLayout rows;
  u16 wrapWidth =
      data->Props.WrapWidth ? data->Props.WrapWidth : contentMax;
  LineEdit_Layout(data->Props.State->Edit, wrapWidth, &rows);
  u16 width = 1;
  for (u32 i = 0; i < rows.Count; i++) {
    u32 rowWidth = InputTextWidth(rows.Rows[i]);
    if (rowWidth > width) width = (u16)rowWidth;
  }
  width = (u16)(width + data->Props.InsetLeft + data->Props.InsetRight);
  if (width > constraints.MaxWidth) width = constraints.MaxWidth;
  u16 height = (u16)rows.Count;
  if (height > constraints.MaxHeight) height = constraints.MaxHeight;
  return UiSize{width, height ? height : (u16)1};
}

void PaintInput(UiContext* ui, const UiNode* node, UiCanvas* canvas) {
  InputData* data = (InputData*)node->User;
  UiTextInputState* state = data->Props.State;
  if (!state || !state->Edit) return;
  StrView text = LineEdit_Text(state->Edit);
  LineEditLayout rows;
  u16 wrapWidth =
      data->Props.WrapWidth ? data->Props.WrapWidth : node->Rect.Width;
  LineEdit_Layout(state->Edit, wrapWidth, &rows);
  if (!state->Dragging) {
    if (rows.CursorRow < state->FirstRow) state->FirstRow = rows.CursorRow;
    if (rows.CursorRow >= state->FirstRow + node->Rect.Height)
      state->FirstRow = rows.CursorRow - node->Rect.Height + 1;
  }
  u32 maxFirst = rows.Count > node->Rect.Height ? rows.Count - node->Rect.Height : 0;
  if (state->FirstRow > maxFirst) state->FirstRow = maxFirst;

  u32 selectionStart = 0;
  u32 selectionEnd = 0;
  bool selected =
      LineEdit_SelectionBounds(state->Edit, &selectionStart, &selectionEnd);
  for (u16 screenRow = 0; screenRow < node->Rect.Height; screenRow++) {
    u32 row = state->FirstRow + screenRow;
    if (row >= rows.Count) break;
    StrView line = rows.Rows[row];
    u32 lineStart = rows.Starts[row];
    u32 offset = 0;
    i32 x = 0;
    while (offset < line.Length) {
      u32 byteStart = offset;
      u32 cp = Utf8Decode(line, &offset);
      u32 absoluteStart = lineStart + byteStart;
      u32 absoluteEnd = lineStart + offset;
      Style style =
          selected && absoluteStart < selectionEnd &&
                  absoluteEnd > selectionStart
              ? data->Props.SelectionStyle
              : data->Props.TextStyle;
      UiCanvas_PutChar(canvas, data->Props.InsetLeft + x, screenRow, cp,
                       style);
      x += CharCellWidth(cp);
    }
    u32 lineEnd = lineStart + line.Length;
    if (selected && lineEnd < text.Length && text.Data[lineEnd] == '\n' &&
        selectionStart <= lineEnd && selectionEnd > lineEnd)
      UiCanvas_Fill(canvas,
                    UiRect{(i32)data->Props.InsetLeft + x, screenRow, 1, 1},
                    data->Props.SelectionStyle);
  }
  if (Ui_IsFocused(ui, node->Id) && rows.CursorRow >= state->FirstRow &&
      rows.CursorRow < state->FirstRow + node->Rect.Height)
    UiCanvas_SetCursor(canvas, node->Id, true,
                       data->Props.InsetLeft + rows.CursorCol,
                       rows.CursorRow - state->FirstRow);
}

void NotifyInputChange(InputData* data) {
  if (data->Props.OnChange) data->Props.OnChange(data->Props.User);
}

bool EventInput(UiContext* ui, UiNode* node, const UiEvent& event) {
  InputData* data = (InputData*)node->User;
  UiTextInputState* state = data->Props.State;
  if (!state || !state->Edit) return false;
  LineEdit* edit = state->Edit;
  bool changed = false;
  if (data->Props.OnInteract) data->Props.OnInteract(data->Props.User);

  if (event.Kind == UiEventKind::Character) {
    if (!event.KeyData.Alt && event.KeyData.Codepoint >= 0x20) {
      LineEdit_InsertChar(edit, event.KeyData.Codepoint);
      changed = true;
    }
  } else if (event.Kind == UiEventKind::Paste) {
    LineEdit_InsertText(edit, event.Text);
    changed = true;
  } else if (event.Kind == UiEventKind::Key) {
    switch (event.KeyData.Kind) {
      case KeyKind::Enter:
        if (data->Props.OnSubmit) data->Props.OnSubmit(data->Props.User);
        return true;
      case KeyKind::Newline:
        LineEdit_Newline(edit);
        changed = true;
        break;
      case KeyKind::Backspace:
        LineEdit_Backspace(edit);
        changed = true;
        break;
      case KeyKind::Delete:
        LineEdit_Delete(edit);
        changed = true;
        break;
      case KeyKind::Left:
        LineEdit_Left(edit, event.KeyData.Shift);
        break;
      case KeyKind::Right:
        LineEdit_Right(edit, event.KeyData.Shift);
        break;
      case KeyKind::Up:
        if (event.KeyData.Shift)
          LineEdit_Up(edit, true);
        else if (!LineEdit_Up(edit) && data->Props.OnUpBoundary)
          changed = data->Props.OnUpBoundary(data->Props.User);
        break;
      case KeyKind::Down:
        if (event.KeyData.Shift)
          LineEdit_Down(edit, true);
        else if (!LineEdit_Down(edit) && data->Props.OnDownBoundary)
          changed = data->Props.OnDownBoundary(data->Props.User);
        break;
      case KeyKind::Home:
        LineEdit_Home(edit, event.KeyData.Shift);
        break;
      case KeyKind::End:
        LineEdit_End(edit, event.KeyData.Shift);
        break;
      case KeyKind::WordLeft:
        LineEdit_WordLeft(edit, event.KeyData.Shift);
        break;
      case KeyKind::WordRight:
        LineEdit_WordRight(edit, event.KeyData.Shift);
        break;
      case KeyKind::CtrlW:
        LineEdit_DeleteWordBack(edit);
        changed = true;
        break;
      case KeyKind::CtrlU:
        LineEdit_KillToStart(edit);
        changed = true;
        break;
      case KeyKind::CtrlK:
        LineEdit_KillToEnd(edit);
        changed = true;
        break;
      default:
        return false;
    }
  } else if (event.Kind == UiEventKind::MouseDown ||
             event.Kind == UiEventKind::MouseMove ||
             event.Kind == UiEventKind::MouseUp) {
    if (event.Kind == UiEventKind::MouseMove && !state->Dragging) return false;
    LineEditLayout rows;
    u16 wrapWidth =
        data->Props.WrapWidth ? data->Props.WrapWidth : node->Rect.Width;
    LineEdit_Layout(edit, wrapWidth, &rows);
    i32 localY = event.Y - node->Rect.Y;
    i32 localX = event.X - node->Rect.X - data->Props.InsetLeft;
    if (localY < 0) localY = 0;
    if (localX < 0) localX = 0;
    u32 row = state->FirstRow + (u32)localY;
    u32 offset = LineEdit_HitTest(edit, wrapWidth, row, (u32)localX);
    if (event.Kind == UiEventKind::MouseDown) {
      Ui_RequestFocus(ui, node->Id);
      LineEdit_SetCursor(edit, offset);
      state->Dragging = true;
      Ui_CapturePointer(ui, node->Id);
    } else if (event.Kind == UiEventKind::MouseMove) {
      LineEdit_SetCursor(edit, offset, true);
    } else {
      LineEdit_SetCursor(edit, offset, state->Dragging);
      state->Dragging = false;
      Ui_ReleasePointer(ui, node->Id);
    }
  } else {
    return false;
  }

  if (changed) NotifyInputChange(data);
  return true;
}

void CancelInput(void* persistent) {
  UiTextInputState* state = (UiTextInputState*)persistent;
  if (state) state->Dragging = false;
}

}  // namespace

u32 Ui_BeginRow(UiContext* ui, UiKey key, UiLayoutStyle layout) {
  UiNodeDesc desc = {};
  desc.Kind = UiNodeKind::Row;
  desc.Key = key;
  desc.Layout = layout;
  return Ui_Begin(ui, desc);
}

u32 Ui_BeginColumn(UiContext* ui, UiKey key, UiLayoutStyle layout) {
  UiNodeDesc desc = {};
  desc.Kind = UiNodeKind::Column;
  desc.Key = key;
  desc.Layout = layout;
  return Ui_Begin(ui, desc);
}

u32 Ui_BeginStack(UiContext* ui, UiKey key, UiLayoutStyle layout) {
  UiNodeDesc desc = {};
  desc.Kind = UiNodeKind::Stack;
  desc.Key = key;
  desc.Layout = layout;
  return Ui_Begin(ui, desc);
}

void Ui_EndContainer(UiContext* ui) { Ui_End(ui); }

u32 Ui_StyledText(UiContext* ui, UiKey key, const UiTextSpan* spans,
                  u32 spanCount, UiTextProps props) {
  TextData* data = (TextData*)Ui_FrameAlloc(ui, sizeof(TextData), alignof(TextData));
  data->Count = spanCount;
  data->Wrap = props.Wrap;
  data->Spans =
      (UiTextSpan*)Ui_FrameAlloc(ui, sizeof(UiTextSpan) * spanCount,
                                alignof(UiTextSpan));
  if (spanCount) memcpy(data->Spans, spans, sizeof(UiTextSpan) * spanCount);
  UiNodeDesc desc = {};
  desc.Kind = UiNodeKind::Text;
  desc.Key = key;
  desc.Ops.Measure = MeasureText;
  desc.Ops.Paint = PaintText;
  desc.User = data;
  desc.Layout = props.Layout;
  return Ui_Leaf(ui, desc);
}

u32 Ui_Text(UiContext* ui, UiKey key, StrView text, UiTextProps props) {
  UiTextSpan span{text, props.StyleValue};
  return Ui_StyledText(ui, key, &span, 1, props);
}

u32 Ui_BeginBorder(UiContext* ui, UiKey key, UiBorderProps props) {
  BorderData* data =
      (BorderData*)Ui_FrameAlloc(ui, sizeof(BorderData), alignof(BorderData));
  *data = BorderData{props.StyleValue, props.FillStyle, props.Fill};
  props.Layout.Padding.Left++;
  props.Layout.Padding.Top++;
  props.Layout.Padding.Right++;
  props.Layout.Padding.Bottom++;
  UiNodeDesc desc = {};
  desc.Kind = UiNodeKind::Stack;
  desc.Key = key;
  desc.Ops.Paint = PaintBorder;
  desc.User = data;
  desc.Layout = props.Layout;
  return Ui_Begin(ui, desc);
}

u32 Ui_BeginPanel(UiContext* ui, UiKey key, UiBorderProps props) {
  props.Fill = true;
  return Ui_BeginBorder(ui, key, props);
}

u32 Ui_SelectableList(UiContext* ui, UiKey key, UiListProps props) {
  ListData* data =
      (ListData*)Ui_FrameAlloc(ui, sizeof(ListData), alignof(ListData));
  data->Props = props;
  UiNodeDesc desc = {};
  desc.Kind = UiNodeKind::Custom;
  desc.Key = key;
  desc.Ops.Measure = MeasureList;
  desc.Ops.Paint = PaintList;
  desc.Ops.Event = EventList;
  desc.User = data;
  desc.Flags = UI_NODE_FOCUSABLE;
  desc.Layout = props.Layout;
  return Ui_Leaf(ui, desc);
}

u32 Ui_BeginScrollView(UiContext* ui, UiKey key, UiScrollState* state,
                       UiLayoutStyle layout) {
  UiNodeDesc desc = {};
  desc.Kind = UiNodeKind::ScrollView;
  desc.Key = key;
  desc.Ops.Event = EventScrollView;
  desc.User = state;
  desc.Layout = layout;
  return Ui_Begin(ui, desc);
}

u32 Ui_Scrollbar(UiContext* ui, UiKey key, UiScrollState* state,
                 Style track, Style thumb, UiLayoutStyle layout) {
  ScrollbarData* data = (ScrollbarData*)Ui_FrameAlloc(
      ui, sizeof(ScrollbarData), alignof(ScrollbarData));
  *data = ScrollbarData{state, track, thumb};
  UiNodeDesc desc = {};
  desc.Kind = UiNodeKind::Custom;
  desc.Key = key;
  desc.Ops.Paint = PaintScrollbar;
  desc.Ops.Event = EventScrollbar;
  desc.Ops.Cancel = CancelScrollbar;
  desc.User = data;
  desc.Persistent = state;
  desc.Layout = layout;
  return Ui_Leaf(ui, desc);
}

u32 Ui_TextInput(UiContext* ui, UiKey key, UiTextInputProps props) {
  InputData* data =
      (InputData*)Ui_FrameAlloc(ui, sizeof(InputData), alignof(InputData));
  data->Props = props;
  UiNodeDesc desc = {};
  desc.Kind = UiNodeKind::Custom;
  desc.Key = key;
  desc.Ops.Measure = MeasureInput;
  desc.Ops.Paint = PaintInput;
  desc.Ops.Event = EventInput;
  desc.Ops.Cancel = CancelInput;
  desc.User = data;
  desc.Persistent = props.State;
  desc.Flags = UI_NODE_FOCUSABLE;
  desc.Layout = props.Layout;
  return Ui_Leaf(ui, desc);
}
