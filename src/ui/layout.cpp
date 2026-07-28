#include "ui/layout.h"

namespace {

u16 ClampU16(u32 value, u16 minValue, u16 maxValue) {
  if (maxValue < minValue) maxValue = minValue;
  if (value < minValue) value = minValue;
  if (value > maxValue) value = maxValue;
  return (u16)value;
}

u16 StyleMax(u16 value, u16 fallback) {
  return value ? (value < fallback ? value : fallback) : fallback;
}

u16 Subtract(u16 value, u32 amount) {
  return amount < value ? (u16)(value - amount) : 0;
}

UiConstraints ChildConstraints(const UiNode* node, UiConstraints constraints) {
  u32 horizontal = (u32)node->Layout.Padding.Left + node->Layout.Padding.Right;
  u32 vertical = (u32)node->Layout.Padding.Top + node->Layout.Padding.Bottom;
  UiConstraints child = {
      0,
      Subtract(constraints.MaxWidth, horizontal),
      0,
      Subtract(constraints.MaxHeight, vertical),
  };
  if (node->Kind == UiNodeKind::ScrollView) child.MaxHeight = 0xffffu;
  return child;
}

u16 ClampLength(const UiNode* node, bool width, u32 value, u16 available) {
  u16 minValue = width ? node->Layout.MinWidth : node->Layout.MinHeight;
  u16 maxValue = width ? node->Layout.MaxWidth : node->Layout.MaxHeight;
  if (!maxValue || maxValue > available) maxValue = available;
  if (minValue > maxValue) minValue = maxValue;
  return ClampU16(value, minValue, maxValue);
}

struct MainAllocation {
  u32 Child;
  u16 Size;
  u16 Max;
  u32 Weight;
  bool Fill;
};

struct MainAllocationSet {
  MainAllocation* Items;
  u32 Count;
  u32 Used;
  bool HasFill;
};

MainAllocationSet AllocateMain(UiContext* ui, UiNode* node, bool row,
                               u16 available, u32 visibleCount) {
  MainAllocationSet result = {};
  if (!visibleCount) return result;
  result.Count = visibleCount;
  result.Items =
      ArenaPushArray(&ui->FrameMem, MainAllocation, visibleCount);
  u32 gaps =
      visibleCount > 1 ? (visibleCount - 1u) * node->Layout.Gap : 0;
  u32 fixedUsed = gaps;
  u32 fillMinimum = 0;
  u32 at = 0;
  for (u32 child = node->FirstChild; child != UI_NODE_NONE;
       child = ui->Nodes.Data[child].NextSibling) {
    UiNode* childNode = &ui->Nodes.Data[child];
    if (childNode->Flags & UI_NODE_HIDDEN) continue;
    UiLength length =
        row ? childNode->Layout.Width : childNode->Layout.Height;
    MainAllocation& allocation = result.Items[at++];
    allocation.Child = child;
    allocation.Fill = length.Kind == UiLengthKind::Fill;
    if (allocation.Fill) {
      result.HasFill = true;
      allocation.Weight = length.Value ? length.Value : 1;
      u16 minValue =
          row ? childNode->Layout.MinWidth : childNode->Layout.MinHeight;
      u16 maxValue =
          row ? childNode->Layout.MaxWidth : childNode->Layout.MaxHeight;
      if (!maxValue || maxValue > available) maxValue = available;
      if (minValue > maxValue) minValue = maxValue;
      allocation.Size = minValue;
      allocation.Max = maxValue;
      fillMinimum += minValue;
    } else {
      u16 measured =
          row ? childNode->Measured.Width : childNode->Measured.Height;
      allocation.Size =
          ClampLength(childNode, row, measured, available);
      allocation.Max = allocation.Size;
      fixedUsed += allocation.Size;
    }
  }

  u32 fillAvailable = fixedUsed < available ? available - fixedUsed : 0;
  if (result.HasFill && fillMinimum <= fillAvailable) {
    u32 extra = fillAvailable - fillMinimum;
    while (extra) {
      u64 totalWeight = 0;
      for (u32 i = 0; i < result.Count; i++) {
        MainAllocation& allocation = result.Items[i];
        if (allocation.Fill && allocation.Size < allocation.Max)
          totalWeight += allocation.Weight;
      }
      if (!totalWeight) break;
      u32 roundExtra = extra;
      u64 weightUsed = 0;
      u32 granted = 0;
      for (u32 i = 0; i < result.Count; i++) {
        MainAllocation& allocation = result.Items[i];
        if (!allocation.Fill || allocation.Size >= allocation.Max) continue;
        u64 nextWeight = weightUsed + allocation.Weight;
        u32 before = (u32)(((u64)roundExtra * weightUsed) / totalWeight);
        u32 after = (u32)(((u64)roundExtra * nextWeight) / totalWeight);
        u32 share = after - before;
        u32 capacity = allocation.Max - allocation.Size;
        if (share > capacity) share = capacity;
        allocation.Size = (u16)(allocation.Size + share);
        granted += share;
        weightUsed = nextWeight;
      }
      if (!granted) break;
      extra -= granted;
    }
  } else if (result.HasFill) {
    u32 left = fillAvailable;
    for (u32 i = 0; i < result.Count; i++) {
      MainAllocation& allocation = result.Items[i];
      if (!allocation.Fill) continue;
      if (allocation.Size > left) allocation.Size = (u16)left;
      left -= allocation.Size;
    }
  }

  u32 cursor = 0;
  for (u32 i = 0; i < result.Count; i++) {
    MainAllocation& allocation = result.Items[i];
    u16 left = cursor < available ? (u16)(available - cursor) : 0;
    if (allocation.Size > left) allocation.Size = left;
    cursor += allocation.Size;
    if (i + 1 < result.Count) cursor += node->Layout.Gap;
  }
  result.Used = cursor < available ? cursor : available;
  return result;
}

UiSize MeasureNode(UiContext* ui, u32 index, UiConstraints constraints) {
  UiNode* node = Ui_GetNode(ui, index);
  if (!node || (node->Flags & UI_NODE_HIDDEN)) {
    if (node) node->Measured = {};
    return {};
  }

  constraints.MinWidth =
      node->Layout.MinWidth > constraints.MinWidth ? node->Layout.MinWidth
                                                   : constraints.MinWidth;
  constraints.MinHeight =
      node->Layout.MinHeight > constraints.MinHeight ? node->Layout.MinHeight
                                                     : constraints.MinHeight;
  constraints.MaxWidth = StyleMax(node->Layout.MaxWidth, constraints.MaxWidth);
  constraints.MaxHeight = StyleMax(node->Layout.MaxHeight, constraints.MaxHeight);
  if (node->Layout.Width.Kind == UiLengthKind::Cells) {
    u16 width = ClampU16(node->Layout.Width.Value, constraints.MinWidth,
                         constraints.MaxWidth);
    constraints.MinWidth = width;
    constraints.MaxWidth = width;
  }
  if (node->Layout.Height.Kind == UiLengthKind::Cells) {
    u16 height = ClampU16(node->Layout.Height.Value, constraints.MinHeight,
                          constraints.MaxHeight);
    constraints.MinHeight = height;
    constraints.MaxHeight = height;
  }

  UiConstraints childConstraints = ChildConstraints(node, constraints);
  u32 contentWidth = 0;
  u32 contentHeight = 0;
  u32 visibleChildren = 0;

  if (node->FirstChild != UI_NODE_NONE) {
    for (u32 child = node->FirstChild; child != UI_NODE_NONE;
         child = ui->Nodes.Data[child].NextSibling) {
      UiNode* childNode = &ui->Nodes.Data[child];
      if (childNode->Flags & UI_NODE_HIDDEN) continue;
      MeasureNode(ui, child, childConstraints);
      visibleChildren++;
    }

    if (node->Kind == UiNodeKind::Row && visibleChildren) {
      usize mark = ui->FrameMem.Used;
      MainAllocationSet allocations =
          AllocateMain(ui, node, true, childConstraints.MaxWidth,
                       visibleChildren);
      for (u32 i = 0; i < allocations.Count; i++) {
        MainAllocation& allocation = allocations.Items[i];
        if (allocation.Fill) {
          UiConstraints exact = childConstraints;
          exact.MinWidth = allocation.Size;
          exact.MaxWidth = allocation.Size;
          MeasureNode(ui, allocation.Child, exact);
        }
      }
      ArenaRestore(&ui->FrameMem, mark);
    }

    for (u32 child = node->FirstChild; child != UI_NODE_NONE;
         child = ui->Nodes.Data[child].NextSibling) {
      UiNode* childNode = &ui->Nodes.Data[child];
      if (childNode->Flags & UI_NODE_HIDDEN) continue;
      UiSize measured = childNode->Measured;
      if (node->Kind == UiNodeKind::Row) {
        contentWidth += measured.Width;
        if (measured.Height > contentHeight) contentHeight = measured.Height;
      } else if (node->Kind == UiNodeKind::Column) {
        contentHeight += measured.Height;
        if (measured.Width > contentWidth) contentWidth = measured.Width;
      } else {
        if (measured.Width > contentWidth) contentWidth = measured.Width;
        if (measured.Height > contentHeight) contentHeight = measured.Height;
      }
    }
    if (visibleChildren > 1) {
      u32 gaps = (visibleChildren - 1u) * node->Layout.Gap;
      if (node->Kind == UiNodeKind::Row)
        contentWidth += gaps;
      else if (node->Kind == UiNodeKind::Column)
        contentHeight += gaps;
    }
  } else if (node->Ops.Measure) {
    UiSize measured = node->Ops.Measure(ui, node, childConstraints);
    contentWidth = measured.Width;
    contentHeight = measured.Height;
  }

  u32 width =
      contentWidth + node->Layout.Padding.Left + node->Layout.Padding.Right;
  u32 height =
      contentHeight + node->Layout.Padding.Top + node->Layout.Padding.Bottom;
  if (node->Layout.Width.Kind == UiLengthKind::Cells)
    width = node->Layout.Width.Value;
  if (node->Layout.Height.Kind == UiLengthKind::Cells)
    height = node->Layout.Height.Value;

  node->Measured.Width =
      ClampU16(width, constraints.MinWidth, constraints.MaxWidth);
  node->Measured.Height =
      ClampU16(height, constraints.MinHeight, constraints.MaxHeight);
  return node->Measured;
}

u16 LengthFor(const UiNode* node, bool width, UiLength length, u16 measured,
              u16 available) {
  if (length.Kind == UiLengthKind::Cells)
    return ClampLength(node, width, measured, available);
  if (length.Kind == UiLengthKind::Fill)
    return ClampLength(node, width, available, available);
  return ClampLength(node, width, measured, available);
}

i32 AlignOffset(UiAlign align, u16 available, u16 size) {
  if (size >= available || align == UiAlign::Start ||
      align == UiAlign::Stretch)
    return 0;
  if (align == UiAlign::Center) return (available - size) / 2;
  return available - size;
}

void LayoutNode(UiContext* ui, u32 index, UiRect rect, UiRect parentClip) {
  UiNode* node = Ui_GetNode(ui, index);
  if (!node) return;
  node->Rect = rect;
  node->Clip = Ui_Intersect(rect, parentClip);
  if ((node->Flags & UI_NODE_HIDDEN) || node->FirstChild == UI_NODE_NONE)
    return;

  i32 innerX = rect.X + node->Layout.Padding.Left;
  i32 innerY = rect.Y + node->Layout.Padding.Top;
  u16 innerWidth = Subtract(
      rect.Width,
      (u32)node->Layout.Padding.Left + node->Layout.Padding.Right);
  u16 innerHeight = Subtract(
      rect.Height,
      (u32)node->Layout.Padding.Top + node->Layout.Padding.Bottom);

  if (node->Kind == UiNodeKind::Stack || node->Kind == UiNodeKind::Root) {
    for (u32 child = node->FirstChild; child != UI_NODE_NONE;
         child = ui->Nodes.Data[child].NextSibling) {
      UiNode* childNode = &ui->Nodes.Data[child];
      if (childNode->Flags & UI_NODE_HIDDEN) continue;
      u16 width = LengthFor(childNode, true, childNode->Layout.Width,
                            childNode->Measured.Width, innerWidth);
      u16 height = LengthFor(childNode, false, childNode->Layout.Height,
                             childNode->Measured.Height, innerHeight);
      if (node->Layout.AlignItems == UiAlign::Stretch &&
          childNode->Layout.Width.Kind == UiLengthKind::Content)
        width = ClampLength(childNode, true, innerWidth, innerWidth);
      i32 x = innerX + AlignOffset(node->Layout.AlignItems, innerWidth, width);
      i32 y = innerY;
      if (node->Layout.JustifyContent == UiJustify::Center &&
          height < innerHeight)
        y += (innerHeight - height) / 2;
      else if (node->Layout.JustifyContent == UiJustify::End &&
               height < innerHeight)
        y += innerHeight - height;
      x += childNode->Layout.OffsetX;
      y += childNode->Layout.OffsetY;
      LayoutNode(ui, child, UiRect{x, y, width, height}, node->Clip);
    }
    return;
  }

  if (node->Kind == UiNodeKind::ScrollView) {
    u32 child = node->FirstChild;
    if (child == UI_NODE_NONE) return;
    UiNode* childNode = &ui->Nodes.Data[child];
    UiScrollState* scroll = (UiScrollState*)node->User;
    u16 childHeight = childNode->Measured.Height;
    i32 maxOffset = childHeight > innerHeight ? childHeight - innerHeight : 0;
    if (scroll) {
      scroll->MaxOffset = maxOffset;
      if (scroll->Follow) scroll->Offset = maxOffset;
      if (scroll->Offset < 0) scroll->Offset = 0;
      if (scroll->Offset > maxOffset) scroll->Offset = maxOffset;
    }
    i32 offset = scroll ? scroll->Offset : 0;
    LayoutNode(ui, child,
               UiRect{innerX, innerY - offset, innerWidth, childHeight},
               node->Clip);
    return;
  }

  bool row = node->Kind == UiNodeKind::Row;
  u16 mainAvailable = row ? innerWidth : innerHeight;
  u16 crossAvailable = row ? innerHeight : innerWidth;
  u32 visibleCount = 0;
  for (u32 child = node->FirstChild; child != UI_NODE_NONE;
       child = ui->Nodes.Data[child].NextSibling) {
    UiNode* childNode = &ui->Nodes.Data[child];
    if (childNode->Flags & UI_NODE_HIDDEN) continue;
    visibleCount++;
  }
  usize allocationMark = ui->FrameMem.Used;
  MainAllocationSet allocations =
      AllocateMain(ui, node, row, mainAvailable, visibleCount);

  u32 extraGap = 0;
  u32 leading = 0;
  if (!allocations.HasFill && allocations.Used < mainAvailable) {
    u32 freeSpace = mainAvailable - allocations.Used;
    if (node->Layout.JustifyContent == UiJustify::Center)
      leading = freeSpace / 2;
    else if (node->Layout.JustifyContent == UiJustify::End)
      leading = freeSpace;
    else if (node->Layout.JustifyContent == UiJustify::SpaceBetween &&
             visibleCount > 1)
      extraGap = freeSpace / (visibleCount - 1u);
  }

  i32 cursor = (row ? innerX : innerY) + (i32)leading;
  for (u32 i = 0; i < allocations.Count; i++) {
    MainAllocation& allocation = allocations.Items[i];
    UiNode* childNode = &ui->Nodes.Data[allocation.Child];
    u16 mainSize = allocation.Size;
    UiLength crossLength =
        row ? childNode->Layout.Height : childNode->Layout.Width;
    u16 measuredCross =
        row ? childNode->Measured.Height : childNode->Measured.Width;
    u16 crossSize =
        LengthFor(childNode, !row, crossLength, measuredCross, crossAvailable);
    if (node->Layout.AlignItems == UiAlign::Stretch &&
        crossLength.Kind == UiLengthKind::Content)
      crossSize =
          ClampLength(childNode, !row, crossAvailable, crossAvailable);
    i32 cross = (row ? innerY : innerX) +
                AlignOffset(node->Layout.AlignItems, crossAvailable, crossSize);
    i32 main = cursor +
               (row ? childNode->Layout.OffsetX : childNode->Layout.OffsetY);
    cross += row ? childNode->Layout.OffsetY : childNode->Layout.OffsetX;

    UiRect childRect =
        row ? UiRect{main, cross, mainSize, crossSize}
            : UiRect{cross, main, crossSize, mainSize};
    LayoutNode(ui, allocation.Child, childRect, node->Clip);
    cursor += mainSize;
    if (i + 1 < allocations.Count)
      cursor += node->Layout.Gap + extraGap;
  }
  ArenaRestore(&ui->FrameMem, allocationMark);
}

}  // namespace

UiRect Ui_Intersect(UiRect a, UiRect b) {
  i32 left = a.X > b.X ? a.X : b.X;
  i32 top = a.Y > b.Y ? a.Y : b.Y;
  i32 aRight = a.X + a.Width;
  i32 bRight = b.X + b.Width;
  i32 right = aRight < bRight ? aRight : bRight;
  i32 aBottom = a.Y + a.Height;
  i32 bBottom = b.Y + b.Height;
  i32 bottom = aBottom < bBottom ? aBottom : bBottom;
  if (right <= left || bottom <= top) return UiRect{left, top, 0, 0};
  return UiRect{left, top, (u16)(right - left), (u16)(bottom - top)};
}

bool Ui_RectContains(UiRect rect, i32 x, i32 y) {
  return rect.Width && rect.Height && x >= rect.X && y >= rect.Y &&
         x < rect.X + rect.Width && y < rect.Y + rect.Height;
}

void Ui_Layout(UiContext* ui, u16 width, u16 height) {
  FFI(!ui || ui->Building || ui->Nodes.Count == 0);
  UiNode* root = &ui->Nodes.Data[0];
  root->Layout.Width = Ui_Cells(width);
  root->Layout.Height = Ui_Cells(height);
  root->Layout.AlignItems = UiAlign::Stretch;
  UiConstraints constraints{width, width, height, height};
  MeasureNode(ui, 0, constraints);
  LayoutNode(ui, 0, UiRect{0, 0, width, height},
             UiRect{0, 0, width, height});
}
