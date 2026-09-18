#include "tui/internal.h"

#include <string.h>

#include "base/sysinfo.h"

namespace TuiInternal {

const char* kSpinner[10] = {"\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8",
                            "\xe2\xa0\xbc", "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7",
                            "\xe2\xa0\x87", "\xe2\xa0\x8f"};

Style TuiSt(const Tui* t, u8 fg, u8 attr) {
  return Style{t->Color ? fg : COL_DEFAULT, COL_DEFAULT, attr};
}

// Append a C-string to a bounded buffer.
void Put(char* buf, u32 cap, u32* len, const char* s) {
  while (*s && *len < cap - 1) buf[(*len)++] = *s++;
}
void PutView(char* buf, u32 cap, u32* len, StrView s) {
  for (u32 i = 0; i < s.Length && *len < cap - 1; i++)
    buf[(*len)++] = s.Data[i];
}
u32 PutViewCells(char* buf, u32 cap, u32* len, StrView s,
                 u32 maxCells) {
  u32 offset = 0;
  u32 cells = 0;
  while (offset < s.Length) {
    u32 start = offset;
    u32 cp = Utf8Decode(s, &offset);
    u32 width = CharCellWidth(cp);
    if (cells + width > maxCells || *len + offset - start >= cap)
      break;
    for (u32 i = start; i < offset; i++) buf[(*len)++] = s.Data[i];
    cells += width;
  }
  return cells;
}
void PutNum(char* buf, u32 cap, u32* len, u64 v) {
  char tmp[20];
  u32 n = 0;
  if (v == 0) tmp[n++] = '0';
  while (v) {
    tmp[n++] = (char)('0' + v % 10);
    v /= 10;
  }
  while (n && *len < cap - 1) buf[(*len)++] = tmp[--n];
}

// Format a count using best-fit binary units and one decimal place.
void PutScaled(char* buf, u32 cap, u32* len, u64 v) {
  const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  u32 unit = 0;
  u64 divisor = 1;
  while (unit + 1 < sizeof(units) / sizeof(units[0]) &&
         v / divisor >= 1024) {
    divisor *= 1024;
    unit++;
  }
  if (unit == 0) {
    PutNum(buf, cap, len, v);
  } else {
    u64 scaled10 = (v / divisor) * 10;
    scaled10 += ((v % divisor) * 10 + divisor / 2) / divisor;
    PutNum(buf, cap, len, scaled10 / 10);
    Put(buf, cap, len, ".");
    char d[2] = {(char)('0' + scaled10 % 10), 0};
    Put(buf, cap, len, d);
  }
  Put(buf, cap, len, units[unit]);
}

// Half-cell resolution, so each of the 16 steps is ~6% of the window. Any
// nonzero occupancy shows at least one step: an empty bar means unknown.
void PutContextBar(char* buf, u32 cap, u32* len, u32 tokens, u32 window,
                   u32* fillEnd) {
  constexpr u64 kSteps = kContextBarCells * 2;
  u64 halves = ((u64)tokens * kSteps + window / 2) / window;
  if (halves > kSteps) halves = kSteps;
  if (tokens && halves == 0) halves = 1;
  for (u64 i = 0; i < halves / 2; i++) Put(buf, cap, len, "\xe2\x94\x81");
  if (halves % 2) Put(buf, cap, len, "\xe2\x95\xb8");
  *fillEnd = *len;
  for (u64 i = (halves + 1) / 2; i < kContextBarCells; i++)
    Put(buf, cap, len, "\xe2\x94\x80");
}

// ---- queue ----

void FormatStatus(Tui* t, char* left, u32 leftCap, u32* ll, char* right,
                  u32 rightCap, u32* rl, u16 cols, TuiStatusBar* bar) {
  *bar = {};
  *ll = 0;
  if (t->YoloWarningActive) {
    Put(left, leftCap, ll, "YOLO enabled: Clanker permission checks disabled");
  } else if (t->CopyNoticeActive) {
    Put(left, leftCap, ll, "copied ");
    PutNum(left, leftCap, ll, t->CopyNoticeChars);
    Put(left, leftCap, ll, t->CopyNoticeChars == 1 ? " char" : " chars");
  } else if (t->OperationActive) {
    Put(left, leftCap, ll, kSpinner[t->Spinner % 10]);
    Put(left, leftCap, ll, " ");
    for (u32 i = 0; i < t->OperationLabel.Text.Length && *ll < leftCap - 1; i++)
      left[(*ll)++] = t->OperationLabel.Text.Data[i];
    if (t->OperationStatus.Text.Length) {
      Put(left, leftCap, ll, ": ");
      for (u32 i = 0; i < t->OperationStatus.Text.Length && *ll < leftCap - 1; i++)
        left[(*ll)++] = t->OperationStatus.Text.Data[i];
    }
  } else if (t->State == TuiState::Idle) {
    Put(left, leftCap, ll, "ready");
  } else {
    Put(left, leftCap, ll, kSpinner[t->Spinner % 10]);
    Put(left, leftCap, ll, " ");
    if (t->State == TuiState::Tool) {
      Put(left, leftCap, ll, "running ");
      for (u32 i = 0; i < t->StateTool.Text.Length && *ll < leftCap - 1; i++)
        left[(*ll)++] = t->StateTool.Text.Data[i];
    } else if (t->State == TuiState::Retry) {
      Put(left, leftCap, ll, "retrying ");
      for (u32 i = 0; i < t->RetryStatus.Text.Length && *ll < leftCap - 1; i++)
        left[(*ll)++] = t->RetryStatus.Text.Data[i];
    } else if (t->State == TuiState::PermissionReview) {
      Put(left, leftCap, ll, "reviewing ");
      for (u32 i = 0; i < t->StateTool.Text.Length && *ll < leftCap - 1; i++)
        left[(*ll)++] = t->StateTool.Text.Data[i];
      Put(left, leftCap, ll, " permission");
    } else if (t->State == TuiState::Streaming) {
      Put(left, leftCap, ll, "responding");
    } else {
      Put(left, leftCap, ll, "thinking");
    }
  }

  *rl = 0;
  u32 rightLimit = cols;
  if (rightLimit > rightCap - 1) rightLimit = rightCap - 1;
  StrView permission =
      t->Permission == TuiPermissionMode::Yolo
          ? SVL("YOLO")
          : t->Permission == TuiPermissionMode::Ask ? SVL("ask")
                                                    : SVL("auto");
  u32 permissionCells = TextWidth(permission);
  u32 modelCells = TextWidth(t->ModelName);
  bool showPermission =
      *ll + permissionCells + modelCells + 4 <= rightLimit;
  u32 rightCells = 0;
  if (showPermission) {
    rightCells =
        PutViewCells(right, rightCap, rl, permission, rightLimit);
    Put(right, rightCap, rl, " \xc2\xb7 ");
    rightCells += 3;
  }
  rightCells += PutViewCells(right, rightCap, rl, t->ModelName,
                             rightLimit > rightCells
                                 ? rightLimit - rightCells
                                 : 0);

  char usage[64] = {};
  u32 usageLength = 0;
  if (t->Use.Input || t->Use.Output) {
    Put(usage, 64, &usageLength, "  \xe2\x86\x91");
    PutScaled(usage, 64, &usageLength, t->Use.Input);
    Put(usage, 64, &usageLength, " \xe2\x86\x93");
    PutScaled(usage, 64, &usageLength, t->Use.Output);
  }

  char effort[24] = {};
  u32 effortLength = 0;
  if (t->Profile.Effort != ModelEffort::None) {
    Put(effort, 24, &effortLength, "  e:");
    PutView(effort, 24, &effortLength,
            ModelEffort_Name(t->Profile.Effort));
  }
  u32 usageCells = TextWidth(StrView{usage, usageLength});
  u32 effortCells = TextWidth(StrView{effort, effortLength});

  // Narrow rows shed segments in reverse priority: effort, then token usage,
  // then the context bar. The model name always stays.
  constexpr StrView kContextLabel = SVL("  ctx ");
  u32 window = t->Profile.ContextWindow;
  u32 contextCells = window ? kContextLabel.Length + kContextBarCells : 0;
  bool showContext = window && rightCells + contextCells <= rightLimit;
  if (!showContext) contextCells = 0;
  bool showUsage = rightCells + contextCells + usageCells <= rightLimit;
  if (!showUsage) usageCells = 0;
  if (rightCells + effortCells + contextCells + usageCells <= rightLimit)
    PutView(right, rightCap, rl, StrView{effort, effortLength});

  if (showContext) {
    PutView(right, rightCap, rl, kContextLabel);
    bar->Start = *rl;
    PutContextBar(right, rightCap, rl, t->ContextTokens, window, &bar->FillEnd);
    bar->End = *rl;
    u64 used = (u64)t->ContextTokens * 10;
    bar->FillFg = used > (u64)window * 9   ? CONTEXT_FULL_FG
                  : used > (u64)window * 7 ? CONTEXT_WARN_FG
                                           : CONTEXT_FILL_FG;
  }

  if (showUsage) PutView(right, rightCap, rl, StrView{usage, usageLength});
}

void BuildStatusComponents(Tui* t, u16 statusY, u16 cols) {
  char* left = (char*)Ui_FrameAlloc(&t->Ui, 256, alignof(char));
  char* right = (char*)Ui_FrameAlloc(&t->Ui, 256, alignof(char));
  u32 leftLength = 0;
  u32 rightLength = 0;
  // The row is inset on both sides, so the right-aligned group clears the
  // transcript scrollbar column. Format against the usable width, not `cols`.
  constexpr u16 kStatusInset = 1;
  u16 contentCols = cols > 2 * kStatusInset ? (u16)(cols - 2 * kStatusInset) : 0;
  TuiStatusBar bar = {};
  FormatStatus(t, left, 256, &leftLength, right, 256, &rightLength, contentCols,
               &bar);

  UiLayoutStyle row = Positioned(0, statusY, cols, 1);
  row.Padding.Left = kStatusInset;
  row.Padding.Right = kStatusInset;
  Ui_BeginRow(&t->Ui, Ui_Key(SVL("status")), row);
  Style style = TuiSt(t, 244, ATTR_NONE);
  UiTextProps leftProps = {};
  leftProps.Layout.Width = Ui_Fill();
  leftProps.Layout.Height = Ui_Fill();
  leftProps.StyleValue =
      t->CopyNoticeActive ? TuiSt(t, COPY_NOTICE_FG, ATTR_NONE) : style;
  Ui_Text(&t->Ui, Ui_Key(SVL("left")), StrView{left, leftLength}, leftProps);
  UiTextProps rightProps = {};
  rightProps.Layout.Width = Ui_Content();
  rightProps.Layout.Height = Ui_Fill();
  rightProps.StyleValue = style;
  UiTextSpan spans[4];
  u32 count = 0;
  auto span = [&](u32 from, u32 to, Style spanStyle) {
    if (to > from)
      spans[count++] = UiTextSpan{StrView{right + from, to - from}, spanStyle};
  };
  if (bar.End > bar.Start) {
    span(0, bar.Start, style);
    span(bar.Start, bar.FillEnd, TuiSt(t, bar.FillFg, ATTR_NONE));
    span(bar.FillEnd, bar.End, TuiSt(t, CONTEXT_TRACK_FG, ATTR_NONE));
    span(bar.End, rightLength, style);
  } else {
    spans[count++] = UiTextSpan{StrView{right, rightLength}, style};
  }
  Ui_StyledText(&t->Ui, Ui_Key(SVL("right")), spans, count, rightProps);
  Ui_EndContainer(&t->Ui);
}

void BuildCwdComponent(Tui* t, u16 cwdY, u16 cols) {
  if (t->Cwd.Length == 0 || cols <= 1) return;
  Style style = TuiSt(t, 244, ATTR_NONE);
  UiTextSpan spans[4];
  u32 count = 0;
  spans[count++] = UiTextSpan{t->Cwd, style};
  if (t->Branch.Length) {
    spans[count++] = UiTextSpan{SVL("  (\xe2\x8e\x87 "), style};
    spans[count++] = UiTextSpan{t->Branch, style};
    spans[count++] = UiTextSpan{SVL(")"), style};
  }
  UiTextProps props = {};
  props.Layout = Positioned(1, cwdY, (u16)(cols - 1), 1);
  props.StyleValue = style;
  Ui_StyledText(&t->Ui, Ui_Key(SVL("cwd")), spans, count, props);
}

}  // namespace TuiInternal
