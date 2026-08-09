#pragma once
#include "base/arena.h"
#include "base/array.h"
#include "base/str.h"

struct Screen;
struct UiCanvas;
struct UiContext;
struct UiEvent;
struct UiNode;

using UiId = u64;

constexpr u32 UI_NODE_NONE = 0xffffffffu;

enum class UiNodeKind : u8 {
  Root,
  Row,
  Column,
  Stack,
  ScrollView,
  Text,
  Custom,
};

enum class UiKeyKind : u8 {
  None,
  Integer,
  String,
};

// Rounded is zero so Ui_Init's memset preserves the default.
enum class UiBorderCorners : u8 {
  Rounded,
  Square,
};

struct UiKey {
  UiKeyKind Kind;
  u64 Integer;
  StrView String;
};

inline UiKey Ui_Key(u64 value) {
  UiKey key = {};
  key.Kind = UiKeyKind::Integer;
  key.Integer = value;
  return key;
}

inline UiKey Ui_Key(StrView value) {
  UiKey key = {};
  key.Kind = UiKeyKind::String;
  key.String = value;
  return key;
}

struct UiSize {
  u16 Width;
  u16 Height;
};

struct UiRect {
  i32 X;
  i32 Y;
  u16 Width;
  u16 Height;
};

struct UiConstraints {
  u16 MinWidth;
  u16 MaxWidth;
  u16 MinHeight;
  u16 MaxHeight;
};

enum class UiLengthKind : u8 {
  Content,
  Cells,
  Fill,
};

struct UiLength {
  UiLengthKind Kind;
  u16 Value;
};

inline UiLength Ui_Content() { return UiLength{UiLengthKind::Content, 0}; }
inline UiLength Ui_Cells(u16 cells) {
  return UiLength{UiLengthKind::Cells, cells};
}
inline UiLength Ui_Fill(u16 weight = 1) {
  return UiLength{UiLengthKind::Fill, weight ? weight : (u16)1};
}

struct UiInsets {
  u16 Left;
  u16 Top;
  u16 Right;
  u16 Bottom;
};

enum class UiAlign : u8 {
  Start,
  Center,
  End,
  Stretch,
};

enum class UiJustify : u8 {
  Start,
  Center,
  End,
  SpaceBetween,
};

struct UiLayoutStyle {
  UiLength Width;
  UiLength Height;
  u16 MinWidth;
  u16 MaxWidth;
  u16 MinHeight;
  u16 MaxHeight;
  UiInsets Padding;
  u16 Gap;
  UiAlign AlignItems;
  UiJustify JustifyContent;
  i16 OffsetX;
  i16 OffsetY;
};

using UiMeasureFn = UiSize (*)(UiContext* ui, UiNode* node,
                               UiConstraints constraints);
using UiPaintFn = void (*)(UiContext* ui, const UiNode* node,
                           UiCanvas* canvas);
using UiEventFn = bool (*)(UiContext* ui, UiNode* node,
                           const UiEvent& event);
using UiCancelFn = void (*)(void* persistent);

struct UiNodeOps {
  UiMeasureFn Measure;
  UiPaintFn Paint;
  UiEventFn Event;
  UiCancelFn Cancel;
};

enum : u32 {
  UI_NODE_FOCUSABLE = 1u << 0,
  UI_NODE_DISABLED = 1u << 1,
  UI_NODE_CLIP = 1u << 2,
  UI_NODE_HIDDEN = 1u << 3,
  UI_NODE_FOCUS_SCOPE = 1u << 4,
};

struct UiNodeDesc {
  UiNodeKind Kind;
  UiKey Key;
  UiNodeOps Ops;
  void* User;
  u32 Flags;
  UiLayoutStyle Layout;
  void* Persistent;
};

struct UiNode {
  UiId Id;
  UiNodeKind Kind;
  u32 Parent;
  u32 FirstChild;
  u32 LastChild;
  u32 NextSibling;
  u32 Flags;
  UiNodeOps Ops;
  void* User;
  void* Persistent;
  UiLayoutStyle Layout;
  UiSize Measured;
  UiRect Rect;
  UiRect Clip;
};

struct UiBuildFrame {
  u32 Node;
  u32 SeedDepth;
};

struct UiHitRegion {
  UiId Id;
  UiId Scope;
  u32 Node;
  UiRect Rect;
};

struct UiFocusEntry {
  UiId Id;
  UiId Scope;
  u32 Node;
};

struct UiScrollState {
  i32 Offset;
  i32 MaxOffset;
  bool Follow;
  bool Dragging;
  i32 DragOffset;
};

struct UiMetrics {
  u32 NodeCount;
  u32 HitRegionCount;
  usize FrameBytes;
};

struct UiScopeState {
  UiId Scope;
  UiId Focus;
};

struct UiContext {
  Arena FrameMem;
  bool FrameMemUp;
  bool Building;
  Screen* Target;
  u64 FrameNumber;
  UiBorderCorners BorderCorners;

  Array<UiNode> Nodes;
  UiBuildFrame BuildStack[128];
  u32 BuildDepth;
  UiId SeedStack[256];
  u32 SeedDepth;

  bool CursorRequested;
  UiId CursorOwner;
  i32 CursorX;
  i32 CursorY;

  Array<UiHitRegion> HitRegions;
  Array<UiFocusEntry> FocusEntries;
  UiId Focused;
  UiId Hovered;
  UiId Active;
  UiId Captured;
  UiId ActiveScope;
  UiId RequestedScope;
  UiScopeState ScopeStack[32];
  u32 ScopeDepth;
  UiCancelFn CaptureCancel;
  void* CaptureCancelUser;
};

void Ui_Init(UiContext* ui);
void Ui_Release(UiContext* ui);

void Ui_BeginFrame(UiContext* ui, Screen* target);
void Ui_EndFrame(UiContext* ui);

void* Ui_FrameAlloc(UiContext* ui, usize size, usize align);

void Ui_PushId(UiContext* ui, UiKey key);
void Ui_PopId(UiContext* ui);

u32 Ui_Begin(UiContext* ui, UiNodeDesc desc);
u32 Ui_Leaf(UiContext* ui, UiNodeDesc desc);
void Ui_End(UiContext* ui);

UiNode* Ui_GetNode(UiContext* ui, u32 index);
const UiNode* Ui_GetNode(const UiContext* ui, u32 index);
UiId Ui_CurrentSeed(const UiContext* ui);
UiId Ui_DeriveId(UiId seed, UiKey key);

void Ui_ActivateScope(UiContext* ui, u32 node);
UiMetrics Ui_GetMetrics(const UiContext* ui);
