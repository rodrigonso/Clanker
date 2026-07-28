#include "check.h"
#include "term/screen.h"
#include "ui/canvas.h"
#include "ui/event.h"
#include "ui/layout.h"
#include "ui/ui.h"

namespace {

struct PaintBox {
  u32 Ch;
  UiSize Intrinsic;
  bool Cursor;
  bool Capture;
  u32 EventCount;
  UiEventKind LastEvent;
  u32 CancelCount;
};

UiSize MeasureBox(UiContext*, UiNode* node, UiConstraints) {
  return ((PaintBox*)node->User)->Intrinsic;
}

void PaintBoxNode(UiContext*, const UiNode* node, UiCanvas* canvas) {
  PaintBox* box = (PaintBox*)node->User;
  for (u16 y = 0; y < node->Rect.Height; y++)
    for (u16 x = 0; x < node->Rect.Width; x++)
      UiCanvas_PutChar(canvas, x, y, box->Ch, StyleDefault());
  if (box->Cursor)
    UiCanvas_SetCursor(canvas, node->Id, true, 0, 0);
}

bool EventBoxNode(UiContext* ui, UiNode* node, const UiEvent& event) {
  PaintBox* box = (PaintBox*)node->User;
  box->EventCount++;
  box->LastEvent = event.Kind;
  if (box->Capture && event.Kind == UiEventKind::MouseDown)
    Ui_CapturePointer(ui, node->Id);
  if (box->Capture && event.Kind == UiEventKind::MouseUp)
    Ui_ReleasePointer(ui, node->Id);
  return true;
}

void CancelBox(void* persistent) {
  PaintBox* box = (PaintBox*)persistent;
  if (box) box->CancelCount++;
}

UiNodeDesc BoxDesc(UiKey key, PaintBox* box, UiLayoutStyle layout,
                   bool interactive = false) {
  UiNodeDesc desc = {};
  desc.Kind = UiNodeKind::Custom;
  desc.Key = key;
  desc.Ops.Measure = MeasureBox;
  desc.Ops.Paint = PaintBoxNode;
  if (interactive) {
    desc.Ops.Event = EventBoxNode;
    desc.Ops.Cancel = CancelBox;
    desc.Persistent = box;
  }
  desc.User = box;
  desc.Layout = layout;
  return desc;
}

}  // namespace

void TestUi() {
  UiContext ui;
  Ui_Init(&ui);

  Ui_BeginFrame(&ui, nullptr);
  u32 row = Ui_Begin(&ui, UiNodeDesc{UiNodeKind::Row, Ui_Key(1), {}, nullptr, 0});
  u32 first =
      Ui_Leaf(&ui, UiNodeDesc{UiNodeKind::Text, Ui_Key(SVL("first")), {}, nullptr, 0});
  Ui_PushId(&ui, Ui_Key(99));
  u32 scoped =
      Ui_Leaf(&ui, UiNodeDesc{UiNodeKind::Text, Ui_Key(SVL("first")), {}, nullptr, 0});
  Ui_PopId(&ui);
  Ui_End(&ui);
  Ui_EndFrame(&ui);

  const UiNode* root = Ui_GetNode(&ui, 0);
  const UiNode* rowNode = Ui_GetNode(&ui, row);
  const UiNode* firstNode = Ui_GetNode(&ui, first);
  const UiNode* scopedNode = Ui_GetNode(&ui, scoped);
  CHECK_ALL(ui.FrameNumber == 1, ui.Nodes.Count == 4,
            root && root->FirstChild == row && root->LastChild == row,
            rowNode && rowNode->Parent == 0 && rowNode->FirstChild == first &&
                rowNode->LastChild == scoped,
            firstNode && firstNode->NextSibling == scoped,
            scopedNode && scopedNode->NextSibling == UI_NODE_NONE,
            firstNode->Id != scopedNode->Id);
  UiMetrics metrics = Ui_GetMetrics(&ui);
  CHECK_ALL(metrics.NodeCount == 4, metrics.FrameBytes > 0);

  UiId rowId = rowNode->Id;
  UiId firstId = firstNode->Id;
  usize firstFrameUsed = ui.FrameMem.Used;

  Ui_BeginFrame(&ui, nullptr);
  u32 row2 = Ui_Begin(&ui, UiNodeDesc{UiNodeKind::Row, Ui_Key(1), {}, nullptr, 0});
  u32 first2 =
      Ui_Leaf(&ui, UiNodeDesc{UiNodeKind::Text, Ui_Key(SVL("first")), {}, nullptr, 0});
  Ui_End(&ui);
  Ui_EndFrame(&ui);

  CHECK_ALL(ui.FrameNumber == 2, ui.Nodes.Count == 3,
            Ui_GetNode(&ui, row2)->Id == rowId,
            Ui_GetNode(&ui, first2)->Id == firstId,
            ui.FrameMem.Used <= firstFrameUsed);

  Ui_BeginFrame(&ui, nullptr);
  u32 column =
      Ui_Begin(&ui, UiNodeDesc{UiNodeKind::Column, Ui_Key(2), {}, nullptr, 0});
  u32 nested =
      Ui_Leaf(&ui, UiNodeDesc{UiNodeKind::Text, Ui_Key(SVL("first")), {}, nullptr, 0});
  Ui_End(&ui);
  Ui_EndFrame(&ui);
  CHECK_ALL(Ui_GetNode(&ui, column)->Id != rowId,
            Ui_GetNode(&ui, nested)->Id != firstId);

  Ui_Release(&ui);

  Arena screenArena = ArenaCreate();
  Screen screen;
  Screen_Init(&screen, &screenArena, nullptr, nullptr);
  Ui_Init(&ui);

  PaintBox a{'A', UiSize{4, 1}, false, false, 0, UiEventKind::None};
  PaintBox b{'B', UiSize{1, 1}, true, true, 0, UiEventKind::None};
  PaintBox c{'C', UiSize{1, 1}, false, false, 0, UiEventKind::None};
  UiLayoutStyle rowLayout = {};
  rowLayout.Width = Ui_Fill();
  rowLayout.Height = Ui_Fill();
  rowLayout.Padding = UiInsets{1, 1, 1, 1};
  rowLayout.Gap = 1;
  rowLayout.AlignItems = UiAlign::Stretch;

  Ui_BeginFrame(&ui, &screen);
  UiNodeDesc rowDesc = {};
  rowDesc.Kind = UiNodeKind::Row;
  rowDesc.Key = Ui_Key(1);
  rowDesc.Layout = rowLayout;
  u32 layoutRow = Ui_Begin(&ui, rowDesc);

  UiLayoutStyle fixed = {};
  fixed.Width = Ui_Cells(4);
  fixed.Height = Ui_Fill();
  UiNodeDesc aDesc = BoxDesc(Ui_Key(1), &a, fixed, true);
  aDesc.Flags = UI_NODE_FOCUSABLE;
  u32 aNode = Ui_Leaf(&ui, aDesc);
  UiLayoutStyle fillOne = {};
  fillOne.Width = Ui_Fill(1);
  fillOne.Height = Ui_Fill();
  UiNodeDesc bDesc = BoxDesc(Ui_Key(2), &b, fillOne, true);
  bDesc.Flags = UI_NODE_FOCUSABLE;
  u32 bNode = Ui_Leaf(&ui, bDesc);
  UiLayoutStyle fillTwo = {};
  fillTwo.Width = Ui_Fill(2);
  fillTwo.Height = Ui_Fill();
  u32 cNode = Ui_Leaf(&ui, BoxDesc(Ui_Key(3), &c, fillTwo, true));
  Ui_End(&ui);
  Ui_EndFrame(&ui);
  Ui_Render(&ui, 20, 5);

  CHECK_ALL(Ui_GetNode(&ui, layoutRow)->Rect.Width == 20,
            Ui_GetNode(&ui, layoutRow)->Rect.Height == 5,
            Ui_GetNode(&ui, aNode)->Rect.X == 1,
            Ui_GetNode(&ui, aNode)->Rect.Width == 4,
            Ui_GetNode(&ui, bNode)->Rect.X == 6,
            Ui_GetNode(&ui, bNode)->Rect.Width == 4,
            Ui_GetNode(&ui, cNode)->Rect.X == 11,
            Ui_GetNode(&ui, cNode)->Rect.Width == 8,
            Ui_GetNode(&ui, cNode)->Rect.Height == 3,
            Screen_BackAt(&screen, 0, 0).Ch == 0,
            Screen_BackAt(&screen, 1, 1).Ch == 'A',
            Screen_BackAt(&screen, 6, 1).Ch == 'B',
            Screen_BackAt(&screen, 18, 3).Ch == 'C',
            Screen_BackAt(&screen, 19, 4).Ch == 0,
            screen.CursorVisible && screen.CursorX == 6 && screen.CursorY == 1);

  CHECK(Ui_IsFocused(&ui, Ui_GetNode(&ui, aNode)->Id));
  Key tab = {};
  tab.Kind = KeyKind::Tab;
  CHECK(Ui_Dispatch(&ui, Ui_EventFromKey(tab)));
  CHECK(Ui_IsFocused(&ui, Ui_GetNode(&ui, bNode)->Id));

  UiEvent down = {};
  down.Kind = UiEventKind::MouseDown;
  down.X = 6;
  down.Y = 1;
  CHECK(Ui_Dispatch(&ui, down));
  CHECK_ALL(Ui_IsFocused(&ui, Ui_GetNode(&ui, bNode)->Id),
            Ui_IsActive(&ui, Ui_GetNode(&ui, bNode)->Id),
            ui.Captured == Ui_GetNode(&ui, bNode)->Id,
            b.EventCount == 1 && b.LastEvent == UiEventKind::MouseDown);

  UiEvent move = {};
  move.Kind = UiEventKind::MouseMove;
  move.X = 18;
  move.Y = 3;
  CHECK(Ui_Dispatch(&ui, move));
  CHECK_ALL(b.EventCount == 2 && b.LastEvent == UiEventKind::MouseMove,
            Ui_IsHovered(&ui, Ui_GetNode(&ui, cNode)->Id));

  UiEvent up = move;
  up.Kind = UiEventKind::MouseUp;
  CHECK(Ui_Dispatch(&ui, up));
  CHECK_ALL(b.EventCount == 3 && b.LastEvent == UiEventKind::MouseUp,
            ui.Captured == 0, ui.Active == 0);

  UiId baseFocus = Ui_GetNode(&ui, bNode)->Id;
  PaintBox overlay{'O', UiSize{5, 2}, false, false, 0, UiEventKind::None};
  Ui_BeginFrame(&ui, &screen);
  rowDesc = {};
  rowDesc.Kind = UiNodeKind::Row;
  rowDesc.Key = Ui_Key(1);
  rowDesc.Layout = rowLayout;
  Ui_Begin(&ui, rowDesc);
  aNode = Ui_Leaf(&ui, aDesc);
  bNode = Ui_Leaf(&ui, bDesc);
  Ui_End(&ui);
  UiNodeDesc scopeDesc = {};
  scopeDesc.Kind = UiNodeKind::Stack;
  scopeDesc.Key = Ui_Key(SVL("overlay"));
  scopeDesc.Flags = UI_NODE_FOCUS_SCOPE;
  scopeDesc.Layout.Width = Ui_Fill();
  scopeDesc.Layout.Height = Ui_Fill();
  u32 scope = Ui_Begin(&ui, scopeDesc);
  UiNodeDesc overlayDesc = BoxDesc(Ui_Key(1), &overlay, fixed, true);
  overlayDesc.Flags = UI_NODE_FOCUSABLE;
  u32 overlayNode = Ui_Leaf(&ui, overlayDesc);
  Ui_End(&ui);
  Ui_ActivateScope(&ui, scope);
  Ui_EndFrame(&ui);
  Ui_Render(&ui, 20, 5);
  UiId overlayScopeId = Ui_GetNode(&ui, scope)->Id;
  CHECK_ALL(ui.ActiveScope == Ui_GetNode(&ui, scope)->Id,
            Ui_IsFocused(&ui, Ui_GetNode(&ui, overlayNode)->Id));

  Ui_BeginFrame(&ui, &screen);
  rowDesc = {};
  rowDesc.Kind = UiNodeKind::Row;
  rowDesc.Key = Ui_Key(1);
  rowDesc.Layout = rowLayout;
  Ui_Begin(&ui, rowDesc);
  aNode = Ui_Leaf(&ui, aDesc);
  bNode = Ui_Leaf(&ui, bDesc);
  Ui_End(&ui);
  Ui_EndFrame(&ui);
  Ui_Render(&ui, 20, 5);
  CHECK_ALL(ui.ActiveScope != overlayScopeId,
            ui.Focused == baseFocus,
            Ui_GetNode(&ui, bNode)->Id == baseFocus);

  PaintBox outer{'U', UiSize{3, 1}, false, false, 0, UiEventKind::None};
  PaintBox inner{'I', UiSize{3, 1}, false, false, 0, UiEventKind::None};
  UiNodeDesc outerFocusDesc = BoxDesc(Ui_Key(1), &outer, fixed, true);
  outerFocusDesc.Flags = UI_NODE_FOCUSABLE;
  UiNodeDesc innerFocusDesc = BoxDesc(Ui_Key(1), &inner, fixed, true);
  innerFocusDesc.Flags = UI_NODE_FOCUSABLE;

  Ui_BeginFrame(&ui, &screen);
  Ui_Begin(&ui, rowDesc);
  aNode = Ui_Leaf(&ui, aDesc);
  bNode = Ui_Leaf(&ui, bDesc);
  Ui_End(&ui);
  scopeDesc.Key = Ui_Key(SVL("outer"));
  u32 outerScope = Ui_Begin(&ui, scopeDesc);
  u32 outerFocus = Ui_Leaf(&ui, outerFocusDesc);
  scopeDesc.Key = Ui_Key(SVL("inner"));
  u32 innerScope = Ui_Begin(&ui, scopeDesc);
  u32 innerFocus = Ui_Leaf(&ui, innerFocusDesc);
  Ui_End(&ui);
  Ui_End(&ui);
  Ui_ActivateScope(&ui, outerScope);
  Ui_EndFrame(&ui);
  Ui_Render(&ui, 20, 5);
  UiId outerFocusId = Ui_GetNode(&ui, outerFocus)->Id;
  CHECK(ui.Focused == outerFocusId);

  Ui_BeginFrame(&ui, &screen);
  Ui_Begin(&ui, rowDesc);
  Ui_Leaf(&ui, aDesc);
  Ui_Leaf(&ui, bDesc);
  Ui_End(&ui);
  scopeDesc.Key = Ui_Key(SVL("outer"));
  outerScope = Ui_Begin(&ui, scopeDesc);
  outerFocus = Ui_Leaf(&ui, outerFocusDesc);
  scopeDesc.Key = Ui_Key(SVL("inner"));
  innerScope = Ui_Begin(&ui, scopeDesc);
  innerFocus = Ui_Leaf(&ui, innerFocusDesc);
  Ui_End(&ui);
  Ui_End(&ui);
  Ui_ActivateScope(&ui, innerScope);
  Ui_EndFrame(&ui);
  Ui_Render(&ui, 20, 5);
  UiId innerFocusId = Ui_GetNode(&ui, innerFocus)->Id;
  CHECK(ui.Focused == innerFocusId);

  Ui_BeginFrame(&ui, &screen);
  Ui_Begin(&ui, rowDesc);
  Ui_Leaf(&ui, aDesc);
  Ui_Leaf(&ui, bDesc);
  Ui_End(&ui);
  scopeDesc.Key = Ui_Key(SVL("outer"));
  outerScope = Ui_Begin(&ui, scopeDesc);
  outerFocus = Ui_Leaf(&ui, outerFocusDesc);
  scopeDesc.Key = Ui_Key(SVL("inner"));
  Ui_Begin(&ui, scopeDesc);
  Ui_Leaf(&ui, innerFocusDesc);
  Ui_End(&ui);
  Ui_End(&ui);
  Ui_ActivateScope(&ui, outerScope);
  Ui_EndFrame(&ui);
  Ui_Render(&ui, 20, 5);
  CHECK_ALL(ui.Focused == outerFocusId,
            Ui_GetNode(&ui, outerFocus)->Id == outerFocusId);

  Ui_BeginFrame(&ui, &screen);
  Ui_Begin(&ui, rowDesc);
  Ui_Leaf(&ui, aDesc);
  bNode = Ui_Leaf(&ui, bDesc);
  Ui_End(&ui);
  Ui_EndFrame(&ui);
  Ui_Render(&ui, 20, 5);
  CHECK_ALL(ui.Focused == baseFocus,
            Ui_GetNode(&ui, bNode)->Id == baseFocus);

  down.X = 6;
  down.Y = 1;
  CHECK(Ui_Dispatch(&ui, down));
  CHECK(ui.Captured == baseFocus);
  Ui_BeginFrame(&ui, &screen);
  Ui_Begin(&ui, rowDesc);
  Ui_Leaf(&ui, aDesc);
  UiNodeDesc disabledB = bDesc;
  disabledB.Flags |= UI_NODE_DISABLED;
  bNode = Ui_Leaf(&ui, disabledB);
  Ui_End(&ui);
  Ui_EndFrame(&ui);
  Ui_Render(&ui, 20, 5);
  CHECK_ALL(ui.Captured == 0, b.CancelCount == 1,
            Ui_GetNode(&ui, bNode)->Id == baseFocus);

  Ui_Release(&ui);
  ArenaRelease(&screenArena);
}
