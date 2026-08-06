#include "ui/ui.h"

#include <string.h>

namespace {

constexpr UiId kRootId = 0xcbf29ce484222325ull;

UiId HashBytes(UiId hash, const void* data, usize size) {
  const u8* bytes = (const u8*)data;
  for (usize i = 0; i < size; i++) {
    hash ^= bytes[i];
    hash *= 1099511628211ull;
  }
  return hash ? hash : 1;
}

u32 AddNode(UiContext* ui, UiNodeDesc desc, bool enter) {
  FFI(!ui || !ui->Building);
  FFI(desc.Key.Kind == UiKeyKind::None);
  FFI(ui->BuildDepth == 0 || ui->SeedDepth == 0);

  u32 parentIndex = ui->BuildStack[ui->BuildDepth - 1].Node;
  UiId id = Ui_DeriveId(Ui_CurrentSeed(ui), desc.Key);

#if !defined(NDEBUG)
  for (u32 child = ui->Nodes.Data[parentIndex].FirstChild;
       child != UI_NODE_NONE; child = ui->Nodes.Data[child].NextSibling)
    FFI(ui->Nodes.Data[child].Id == id);
#endif

  UiNode node = {};
  node.Id = id;
  node.Kind = desc.Kind;
  node.Parent = parentIndex;
  node.FirstChild = UI_NODE_NONE;
  node.LastChild = UI_NODE_NONE;
  node.NextSibling = UI_NODE_NONE;
  node.Flags = desc.Flags;
  node.Ops = desc.Ops;
  node.User = desc.User;
  node.Persistent = desc.Persistent;
  node.Layout = desc.Layout;
  u32 index = ui->Nodes.Count;
  Array_Push(&ui->Nodes, node);

  UiNode* parent = &ui->Nodes.Data[parentIndex];
  if (parent->LastChild == UI_NODE_NONE)
    parent->FirstChild = index;
  else
    ui->Nodes.Data[parent->LastChild].NextSibling = index;
  parent->LastChild = index;

  if (enter) {
    FFI(ui->BuildDepth >= 128 || ui->SeedDepth >= 256);
    ui->BuildStack[ui->BuildDepth++] = UiBuildFrame{index, ui->SeedDepth};
    ui->SeedStack[ui->SeedDepth++] = id;
  }
  return index;
}

}  // namespace

void Ui_Init(UiContext* ui) {
  FFI(!ui);
  memset(ui, 0, sizeof(*ui));
  ui->FrameMem = ArenaCreate();
  ui->FrameMemUp = true;
}

void Ui_Release(UiContext* ui) {
  if (!ui) return;
  FFI(ui->Building);
  if (ui->FrameMemUp) ArenaRelease(&ui->FrameMem);
  memset(ui, 0, sizeof(*ui));
}

void Ui_BeginFrame(UiContext* ui, Screen* target) {
  FFI(!ui || !ui->FrameMemUp || ui->Building);
  ArenaReset(&ui->FrameMem);
  ui->Nodes = Array_Make<UiNode>(&ui->FrameMem, 128);
  ui->Target = target;
  ui->Building = true;
  ui->FrameNumber++;
  ui->BuildDepth = 0;
  ui->SeedDepth = 0;
  ui->CursorRequested = false;
  ui->CursorOwner = 0;
  ui->CursorX = 0;
  ui->CursorY = 0;
  ui->HitRegions = Array_Make<UiHitRegion>(&ui->FrameMem, 64);
  ui->FocusEntries = Array_Make<UiFocusEntry>(&ui->FrameMem, 32);
  ui->RequestedScope = 0;

  UiNode root = {};
  root.Id = kRootId;
  root.Kind = UiNodeKind::Root;
  root.Parent = UI_NODE_NONE;
  root.FirstChild = UI_NODE_NONE;
  root.LastChild = UI_NODE_NONE;
  root.NextSibling = UI_NODE_NONE;
  Array_Push(&ui->Nodes, root);

  ui->BuildStack[ui->BuildDepth++] = UiBuildFrame{0, 0};
  ui->SeedStack[ui->SeedDepth++] = kRootId;
}

void Ui_EndFrame(UiContext* ui) {
  FFI(!ui || !ui->Building);
  FFI(ui->BuildDepth != 1 || ui->SeedDepth != 1);
  ui->Building = false;
}

void* Ui_FrameAlloc(UiContext* ui, usize size, usize align) {
  FFI(!ui || !ui->Building);
  return ArenaAlloc(&ui->FrameMem, size, align);
}

UiId Ui_DeriveId(UiId seed, UiKey key) {
  FFI(key.Kind == UiKeyKind::None);
  u8 tag = (u8)key.Kind;
  UiId hash = HashBytes(seed, &tag, sizeof(tag));
  if (key.Kind == UiKeyKind::Integer)
    return HashBytes(hash, &key.Integer, sizeof(key.Integer));
  return HashBytes(hash, key.String.Data, key.String.Length);
}

UiId Ui_CurrentSeed(const UiContext* ui) {
  FFI(!ui || ui->SeedDepth == 0);
  return ui->SeedStack[ui->SeedDepth - 1];
}

void Ui_PushId(UiContext* ui, UiKey key) {
  FFI(!ui || !ui->Building || ui->SeedDepth >= 256);
  ui->SeedStack[ui->SeedDepth++] = Ui_DeriveId(Ui_CurrentSeed(ui), key);
}

void Ui_PopId(UiContext* ui) {
  FFI(!ui || !ui->Building || ui->SeedDepth <= 1);
  u32 nodeSeedDepth = ui->BuildStack[ui->BuildDepth - 1].SeedDepth + 1;
  FFI(ui->SeedDepth <= nodeSeedDepth);
  ui->SeedDepth--;
}

u32 Ui_Begin(UiContext* ui, UiNodeDesc desc) {
  return AddNode(ui, desc, true);
}

u32 Ui_Leaf(UiContext* ui, UiNodeDesc desc) {
  return AddNode(ui, desc, false);
}

void Ui_End(UiContext* ui) {
  FFI(!ui || !ui->Building || ui->BuildDepth <= 1);
  UiBuildFrame frame = ui->BuildStack[ui->BuildDepth - 1];
  FFI(ui->SeedDepth != frame.SeedDepth + 1);
  ui->BuildDepth--;
  ui->SeedDepth = frame.SeedDepth;
}

UiNode* Ui_GetNode(UiContext* ui, u32 index) {
  return ui && index < ui->Nodes.Count ? &ui->Nodes.Data[index] : nullptr;
}

const UiNode* Ui_GetNode(const UiContext* ui, u32 index) {
  return ui && index < ui->Nodes.Count ? &ui->Nodes.Data[index] : nullptr;
}

void Ui_ActivateScope(UiContext* ui, u32 node) {
  FFI(!ui || !ui->Building || node >= ui->Nodes.Count);
  FFI((ui->Nodes.Data[node].Flags & UI_NODE_FOCUS_SCOPE) == 0);
  ui->RequestedScope = ui->Nodes.Data[node].Id;
}

UiMetrics Ui_GetMetrics(const UiContext* ui) {
  if (!ui) return {};
  return UiMetrics{ui->Nodes.Count, ui->HitRegions.Count, ui->FrameMem.Used};
}
